# break_C

## Attack on policy C (md-style failed disk): result

**Headline.** C holds up once its specification is fixed. Six changes (listed below) made C, CTM and A silent-free in every run. As the spec is written today, C can be broken.

Two of the breaks are not specific to C. They are silent losses in **today's kernel (A, CUR and A2)**, inherited from upstream btrfs.

**What was run.**
- **Model.** `attack_C/c_model.py`, a copy of `policy_model.py` with new switches, all off by default. With them off, it gives the same counters as the original in 8 of 8 configurations (`regress.log`).
- **Modelled:**
  - per-device superblocks with generation, FAILED list and tree-log root;
  - transaction commits (TXN) and fsync log commits (LOG);
  - crashes landing any subset of the superblock writes;
  - mounts that pick the newest superblock, with adversarial tie-breaks;
  - the FAILED-PENDING window and its gate (§1.1/§2.4 of the design);
  - T-fua;
  - the ERRATA 6 rule in four forms;
  - ERRATA 1 as written;
  - "failed disk is never a rebuild source" (§2.3);
  - a replaced disk coming back;
  - crashes during mount recovery;
  - orphan superblocks;
  - T-admin on a healthy disk.
- **Volume.** About 110 exhaustive runs (depth 4–12, up to 1.39 million states each; every run was at nice 10 and took under 30 minutes), plus 5 fixed-schedule availability tables (`sched.py`).

**Breaks in C as specified.**
1. **ERRATA 6 misses devices admitted outside the superblock write.** This covers T-fua on the write-intent log block, T-admin, T-repair and T-log.
   - The failed disk keeps a superblock of the same generation with neither the failed list nor the log root. A mount that picks it silently drops fsync'd writes.
   - Silent states: 26,944 (persistent), 41,852 (with T-fua), 47,276 (admin), 43,012 (flush), 68,796 (transient), 156,992 (RAID6).
   - With the rule that also covers "skipped as already failed" (the model-attack's change 1): 0 in all.
2. **ERRATA 1 as written loses data on a clean remount.** It applies the crash-recovery rule (§3.5) to every loaded record, and "forget" keeps entries as vague sticky records.
   - After one clean remount, the failed disk's nodatasum sectors in every stripe that ever named it read EIO for good, although parity holds the value.
   - 1,392 lost states with no crash at all, against 0. With crashes: 18,872 against 9,336.
   - The stage-0 kernel already restricts this rule to possibly-torn records (scrub.c:5637-5640).
3. **The pending-window gate makes finished writes look torn.** It keeps each finished write's in-flight mark on disk. A mount that lists the failed disk then marks acknowledged, fully written stripes suspect.
   - Replay (`residue_demo.out`): the disk fails, one write to a *healthy* column is acknowledged, crash; the failed disk's untouched sector reads EIO for good.
   - A "finished" flag on gated entries fixes it: lost-but-on-disk states drop from 11,044 to 5,744 and from 10,144 to 7,472; silent stays 0.
4. **T-missing is fooled by an orphan superblock (new).** A transaction commit that crashed after landing only on a device, which was then missing at the next mount, leaves a superblock whose generation the degraded session reuses.
   - That device comes back trusted: 680 silent states with every other fix.
   - The design's witness rule (sb_bound = last committed generation) would also take it for a replacement. This is inferred; the rule is not modelled.
   - Fix: the degraded read-write mount skips a generation (and sb_bound = G+1).
5. **A mount acts on a superblock choice it has not made durable (new).** It forgets names and drops records based on the list of the superblock it picked out of a tie, before that list is written to every present device.
   - The next mount can pick the other superblock: 1,541 silent states (CTM, with every other fix).
   - Fix: commit the picked superblock at mount, before recovery.

With **all fixes** (full F1 rule extended to missing devices, generation skip, mount commit, T-missing, tombstone, §3.5 only on possibly-torn records), C, CTM and A have 0 silent states in every run:
- 1-stripe runs with two crashes and a device missing then back, depth 6: C 359,765 states, CTM 476,801, A 339,350;
- flush-or-write faults at depth 12, 469,492 states;
- metadata at depth 11;
- mixed checksums at depth 8;
- CTM with 2 stripes and log commits at depth 5, 736,852 states.

**C's own costs that remain.** They are loud, not silent, but they are real:
- C-1: degraded write hole on the failed disk (data gone from disk).
- C-2: every crash while degraded makes that disk's nodatasum sectors unreadable.
- C-4: a second loss in RAID5 leaves the filesystem read-only for good.
- C-5: metadata is lost on a crash where A would go read-only.
- RAID6 with two disks failed: 1,912 gone / 59,844 unreachable against A's 0 / 14,568.
- Stage-1's "never a source" rule: failed disk plus second loss makes all of the second disk unreadable, even checksummed data (1,832 against 1,000). One-line trace: fail d0, lose d1.
- T-fua on the log block turns a 6-write glitch into a permanent failure. That contradicts the design's own "transient: no transition" row.

Only strict_degraded plus narrow in-flight marks, plus the missing checksum case of strict_degraded, removes C's losses. That last case was a gap in the earlier refinement: 12,264 gone, now 0. After that, C refuses about 60% more than A+NI in the exhaustive runs (54,580 vs 33,716 refused-write transitions). In the fixed schedules it refuses fewer writes (12 vs 18 of 24).

**Availability, same schedules.** RAID5, 4 full stripes, log of 2, 24 writes, persistent failure of d0:

| Policy | Writes refused | Crash points losing acknowledged data | Gone from disk |
|---|---|---|---|
| A | 18/24 | 18/24 (readable with NI) | 0 |
| A+NI | 18/24 | 0 | 0 |
| C (T-fua) | 1/24 | 90/160 | 22 |
| C strict | 12/24 | 0/72 | 0 |

- Checksummed data: C loses 24/168, all gone from disk; A loses 0.
- RAID5 second loss: C ends read-only with 8 cells EIO; A stays read-write with 4.

C trades refusals for loss.

**Cross-policy findings that apply to the kernel today (A, CUR, A2).**
- **X-1: log-commit superblock tie.** A disk starts failing writes after the last transaction commit; an fsync's log commit misses its superblock; crash. A mount that picks that disk's same-generation superblock has no log root, so the fsync'd writes vanish silently.
  - 59,136 of 186,084 states for A, CUR and A2 alike; 115,568 in RAID6; checksums do not help.
  - The same happens with a device missing at a degraded mount that returns.
  - Kernel lines: tree-log.c:3550-3552; disk-io.c:4198-4206, 4223, 4290, 4314 and 3895; volumes.c:689 and 1241-1248; disk-io.c:3413.
  - Forcing a transaction commit when a log commit's superblock misses any device holding the current generation gives 0.
- **X-2: orphan superblock after a degraded session.** 35,092 silent states for A (32,733 for CUR at depth 5). Fixed by the generation skip plus the mount commit.
- **X-3: a replaced write-failing disk comes back as the member.** 2,534 silent states for A, CUR and A2. C's tombstone gives 0; the no-tombstone control gives 2,710.
  - Kernel lines: dev-replace.c:1058-1059, volumes.c:2249-2268, 902-915 and 943-961.

None of these was reproduced on a UML kernel.

**REPORT.md was not written.** The environment refuses report files from subagents; this output carries the report.

## Table

Cell format: states / depth, then SILENT (of which log-root and orphan), LOST gone, LOST unreachable, STUCK, refused writes (REF_W, distinct transitions), RO (transitions). RAID5 unless noted. "T" means the run stopped on its 2.8 GB memory guard; counts are exact to that depth. Every run is under attack_C/out/.

**A. Superblock tie (1 full stripe, persistent failure of d0 unless noted)**

| Run | States / depth | SILENT (log root) | LOST gone / unreach | REF_W |
|---|---|---|---|---|
| A, F1 none (**CUR and A2 identical**) | 186,084 / 10 | **59,136** (59,136) | 0 / 1,288 | 26,242 |
| A, F1 any (CUR identical) | 34,978 / 10 | 0 | 0 / 1,192 | 8,594 |
| A, csum, F1 none | 184,860 / 10 | **59,136** | 0 / 0 | 25,186 |
| A, transient, F1 none | 333,970 / 10 | **115,422** | 0 / 3,884 | 28,382 |
| A, transient, F1 any | 64,626 / 10 | 0 | 0 / 3,504 | 10,506 |
| A, flush faults | 31,440 / 10 | 0 | 0 / 504 | 504 |
| A, RAID6, F1 none | 283,156 / 9 | **115,568** | 0 / 568 | 24,230 |
| C pending, F1 none | 598,870 / 10 | **159,100** | 1,792 / 16,842 | 76,088 |
| C pending, F1 errata | 272,188 / 10 | **26,944** | 1,400 / 15,270 | 41,398 |
| C pending, F1 full (any: same) | 224,306 / 10 | 0 | 1,400 / 14,666 | 36,684 |
| C pending+T-fua, errata | 240,000 / 10 | **41,852** | 432 / 6,718 | 20,356 |
| C pending+T-fua, full (any: same) | 168,298 / 10 | 0 | 432 / 5,970 | 13,760 |
| C pending+T-fua, transient, errata | 399,854 / 10 | **68,796** | 776 / 12,154 | 35,304 |
| C pending+T-fua, transient, full | 280,446 / 10 | 0 | 776 / 10,786 | 24,122 |
| C pending, flush, errata | 287,374 / 10 | **43,012** | 776 / 8,988 | 17,612 |
| C pending, flush, full | 208,292 / 10 | 0 | 776 / 6,330 | 8,858 |
| C pending, T-admin on a healthy disk, errata | 279,862 / 10 | **47,276** | 9,630 / 16,258 | 36,320 |
| C pending, T-admin, full | 186,719 / 10 | 0 | 7,071 / 11,730 | 21,396 |
| C RAID6, T-fua, errata | 427,106 / 9 | **156,992** | 0 / 3,656 | 3,032 |
| C RAID6, T-fua, full | 225,910 / 9 | 0 | 0 / 3,392 | 2,816 |
| C, 2 stripes, LOG commits, errata | 741,851 / 6 | **113,496** | 3,392 / 86,468 | 61,776 |
| C, 2 stripes, LOG commits, full | 479,123 / 6 | 0 | 3,192 / 58,248 | 35,064 |

**B. A device missing at a crash-mount, then back** (1 stripe, 2 crashes, depth 6, final model; "fixes" = `--f1-missing --gen-skip --mount-commit`)

| Run | States / depth | SILENT | of which log root / orphan / data path | LOST gone / unreach |
|---|---|---|---|---|
| CUR, F1 none | 1,334,948 / 5 T | **103,033** | 70,300 / 32,733 / 0 | 61,438 / 79,745 |
| A, F1 any | 1,343,054 / 6 | **71,926** | 36,834 / 35,092 / 0 | 66,943 / 100,808 |
| A, F1 any + fixes | 339,350 / 6 | 0 | — | 19,825 / 24,452 |
| C pending, full | 1,392,063 / 5 T | **67,973** | 33,463 / 33,778 / 732 | 49,981 / 152,364 |
| C pending, full + fixes | 359,765 / 6 | 0 | — | 18,170 / 50,156 |
| CTM pending, full | 1,131,424 / 5 T | **36,204** | 14,932 / 18,817 / 2,455 | 44,544 / 218,132 |
| CTM + fixes | 476,801 / 6 | 0 | — | 25,096 / 85,702 |
| CTM + fixes, without gen-skip | 573,413 / 6 | **680** | data path | — |
| CTM + fixes, without mount-commit | 1,016,141 / 6 | **1,541** | data path | — |

Group D with every fix, all 0 silent:
- C, flush-or-write faults, LOG commits: 469,492 / 12;
- C, metadata: 214,758 / 11 (768 gone, 360 RO);
- C, mixed checksums, residue fix: 39,348 / 8;
- CTM, 2 stripes, missing then back, LOG commits: 736,852 / 5;
- CTM, RAID6, two failed: 602,536 / 4 T.

Two runs are model-dependent (see caveats):
- C (no T-missing), RAID6, two failed plus a returning disk: 506 silent (784,645 / 5 T).
- A, RAID6: 52 silent.

**C. Pending window** (2 stripes, TXN commits only, F1 full, depth 7)

| Run | States | SILENT | LOST gone / unreach | REF_W | RO |
|---|---|---|---|---|---|
| A (with superblocks) | 48,506 | 0 | 0 / 2,648 | 13,772 | 0 |
| C, atomic forget (retire-early control) | 130,418 | **10,378** | 6,498 / 11,224 | 18,652 | 0 |
| C pending | 142,098 | 0 | 2,320 / **16,884** | 27,776 | 0 |
| C pending + residue fix | 140,822 | 0 | 2,320 / 11,472 | 24,512 | 0 |
| C pending+T-fua | 69,004 | 0 | 1,200 / 10,144 | 12,828 | 0 |
| C pending+T-fua + residue fix | 67,596 | 0 | 1,200 / 7,472 | 10,700 | 0 |
| C pending+T-fua, csum | 61,760 | 0 | 1,296 / 0 | 888 | 0 |
| C transient+T-fua | 113,807 | 0 | 1,840 / 16,944 | 19,396 | 0 |
| A transient | 88,197 | 0 | 0 / 6,672 | 15,596 | 0 |
| C, log of 1, T-fua | 68,180 | 0 | 1,200 / 9,520 | 32,852 | 0 |
| A, log of 1 | 40,051 | 0 | 0 / 1,680 | 35,852 | 0 |
| C, metadata, T-fua | 62,648 | 0 | **2,184** / 0 | 888 | 888 |
| A, metadata | 55,948 | 0 | 0 / 0 | 12,004 | 12,004 |
| C, a second disk failing (RAID5, depth 6) | 178,674 | 0 | 5,184 / 46,884 | 105,368 | 0 |
| A, a second disk failing (RAID5, depth 6) | 95,753 | 0 | 1,284 / 8,436 | 68,128 | 0 |

- The C second-failure run without a spare disk is STUCK in 57,703 states. Any policy with two failing disks and no spare is stuck; the trace is simply "d0 and d1 start failing".
- The detach runs at depth 6 (final model) give 0 silent for C pending (554,113 states) and 2,022 for the no-gate control (545,625 states).

**D. ERRATA 1** (§3.5 on every loaded record; atomic C, 2 stripes)

| Run | States / depth | LOST unreach |
|---|---|---|
| Persistent, no crash, ERRATA 1 as written | 9,033 / 10 | **1,392** |
| Persistent, no crash, torn records only | 3,883 / 10 | 0 |
| Persistent, 1 crash, as written | 63,977 / 9 | 18,872 |
| Persistent, 1 crash, torn only | 32,647 / 9 | 9,336 |
| RAID6, as written | 11,298 / 9 | 0 |
| Flush | 29,041 / 9 | 6,864 either way |

**E. Replaced disk comes back** (2 stripes, depth 7)

| Policy | States | SILENT |
|---|---|---|
| A (CUR and A2 identical) | 384,219 | **2,534** |
| A, csum | 348,255 | 0 |
| C (tombstone) | 454,831 | 0 |
| CTM | 430,923 | 0 |
| C, no-tombstone control | 451,570 | **2,710** |
| C pending with superblocks | 753,482 / 5 T | 0 |

**F. Stage-1 "never a source" (nosrc)**, RAID5 second loss, csum, depth 7:
- C: LOST gone 85,636, unreach 1,000, needless EIO 4,764, STUCK 1,788.
- C+nosrc: gone 85,636, unreach **1,832**, needless EIO 8,216, STUCK 956.

**G. RAID6 with two disks failed** (depth 7)

| Run | LOST gone / unreach | REF_W |
|---|---|---|
| C, nodatasum | 1,912 / 59,844 | 47,080 |
| A, nodatasum | 0 / 14,568 | 29,744 |
| C, csum | 1,696 / 176 | 29,364 |
| A, csum | 0 / 0 | 25,148 |

**H. Strict variants with checksums** (persistent failure, depth 9)

| Run | LOST gone | REF_W |
|---|---|---|
| C + strict_degraded + narrow in-flight | **12,264** | 41,308 |
| … plus the checksum case (`sd_csum`) | 0 | 54,580 |
| A + narrow in-flight | 0 | 33,716 |

**I. Availability under identical fault schedules** (`sched_*.txt`)

Setup: 24 writes round-robin, queued repair every 3 writes, 4 full stripes, log of 2. Each cell: writes refused / crash points losing acknowledged data (of which gone from disk).

| Schedule (nodatasum) | A | A+NI | C T-log | C T-fua | C T-fua strict |
|---|---|---|---|---|---|
| d0 fails for good | 18 / 18 of 24 (0) | 18 / 0 of 24 | 2 / 82 of 88 (20) | 1 / 90 of 160 (22) | 12 / 0 of 72 |
| d0 fails, replaced at write 12 | 8 / 10 of 76 | 8 / 0 | 2 / 34 of 100 (8) | 1 / 42 of 220 (10) | 6 / 0 of 180 |
| parity disk fails | 12 / 0 | 12 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| 6-write glitch on d0 | 6 / 7 of 88 | 6 / 0 | 2 / 82 of 88 (20); disk failed for good | 1 / 90 of 160 (22); disk failed for good | 12 / 0 of 72; disk failed for good |
| d0 fails, d1 lost at write 12 | 20 / 10 of 16; end rw, 4 EIO | 20 / 0 | 14 + RO / 34 of 40 (8); end ro, 8 EIO | 13 + RO / 42 of 76 (10); end ro, 8 EIO | 18 + RO / 0 of 36; end ro, 8 EIO |

Checksummed data, d0 fails for good:
- A and A+NI: 18 refused, 0 lost.
- C T-log: 2 refused, 20 of 88 lost (all gone).
- C T-fua: 0 refused, 24 of 168 lost (all gone).
- C strict: 12 refused, 0 lost.

RAID6 checksummed, d0 fails then d1 is lost: lost crash points A 4 of 36, C 32 of 148 to 296, C strict 0 of 248.

## Counterexamples

### A (identical for CUR and A2): SILENT_WRONG (fsync'd data lost with the log root) (Silent loss of fsync'd data, on healthy disks too. 59,136 of 186,084 states (RAID5), 115,422 transient, 115,568 RAID6; checksummed data is not protected. The only alert is a device write-error message at fsync time. With F1 'any' (force a transaction commit when a log commit's superblock missed a present device holding the current generation): 0. With missing devices also covered (--f1-missing) and gen-skip plus mount-commit: 0 in group M.)

Scenario: A member starts failing writes after the last transaction commit, then an fsync, then a crash before the next transaction commit (S_persist_*_f1none, 1 stripe, depth 10).

Trace: d0 starts failing writes || write s0.d1=2 ; LOG commit ok (the log commit's superblock to d0 fails and is tolerated) || CRASH (before the next transaction commit) ; mount (tie: sb of {d0}) => the superblock picked has generation G and no log root: s0.d1 reads 1 (acknowledged 2), no error. Also shown with a device missing at a degraded mount that later returns (group M).

Kernel: Yes; upstream behaviour kept by stage-0. btrfs_sync_log sets the log root in super_for_commit and calls write_all_supers (tree-log.c:3550-3552). The fsync path writes the primary copy only (disk-io.c:4198-4206) and tolerates num_devices-1 superblock errors (max_errors at disk-io.c:4223; checks at 4290 and 4314); a failed superblock write only prints 'lost super block write due to IO error' (disk-io.c:3895). At mount, btrfs_open_fs_devices picks latest_dev by a strict '>' on device->generation in list order (volumes.c:1241-1248; generation read from each device's superblock, volumes.c:689) and reads that device's superblock (disk-io.c:3413). A write-failing but readable member keeps a same-generation superblock without the log root. Not reproduced on UML. Suggested arm: dm-flakey error_writes on one member after a transaction commit, fsync on another column, panic, mount with the failing device scanned last (first in the list).

### A (identical for CUR and A2): SILENT_WRONG (orphan superblock lineage mounted) (Silent. A with F1 any: 35,092 orphan states plus 36,834 log-root states of 1,343,054; CUR (F1 none, depth 5 T): 32,733 + 70,300. With a generation skip at degraded read-write mounts plus a mount commit: 0.)

Scenario: A transaction commit crashes after its superblock lands only on d0. The next mount does not see d0 (degraded read-write) and continues from generation G. d0 returns after the degraded session has committed or fsync'd (group M, 1 stripe, 2 crashes).

Trace: txn commit ; TXN commit sb landed {d0} ; CRASH ; mount without d0 || write s0.d0=2 ; LOG commit ok || unmount ; mount with {d0} back (tie: sb of {d0}) => ORPHAN SUPERBLOCK: the crashed transaction's G+1 root is mounted; the degraded session's own G+1 commit and its acknowledged writes are gone, with no error

Kernel: Yes; upstream split-brain, inferred from code (the model tracks the superblock pick, not tree contents). write_all_supers accepts a commit with up to num_devices-1 superblock errors (disk-io.c:4223/4290/4314), and a crash inside the superblock loop can land generation G+1 on one device only. A degraded mount without that device reads G (volumes.c:1241-1248, disk-io.c:3413), and its next commit reuses G+1. When the device returns, the tie at G+1 is broken by list order. The session's rewritten tree blocks also carry generation G+1, so transid checks cannot tell the lineages apart.

### A (identical for CUR and A2): SILENT_WRONG (a replaced disk rejoins as the member) (Silent. 2,534 of 384,219 states for A, CUR and A2 (nodatasum); 0 with checksums. C and CTM with the tombstone rule: 0. Control no_tombstone: 2,710.)

Scenario: A write-failing disk is replaced; it stays attached (or is re-attached) and later shows up at a mount where the replacement T is missing (G_*, 2 stripes, depth 7).

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || replace d0 || unmount ; mount without T(d0), the old d0 accepted as devid d0 => s0.d0 reads 1 (acknowledged 2), no record names it (the replace cleared d0's names)

Kernel: Yes; upstream. dev-replace.c:1058-1059 scratches the source superblock only if it is WRITEABLE, and btrfs_scratch_superblock (volumes.c:2249-2268) only warns if that write fails. A write-failing source therefore keeps a valid superblock with devid N and T's uuid. device_list_add rejects a same-devid device only when a newer one is already registered and the filesystem is not open (volumes.c:902-915), and re-adds a 'missing devid' at 943-961. With T absent, the old disk becomes the member.

### C (stage 1 with ERRATA 6 as written): SILENT_WRONG (fsync'd data lost with the log root) (Silent. Persistent 26,944 of 272,188; with T-fua 41,852 of 240,000; T-admin on a healthy disk 47,276; flush 43,012; transient 68,796; RAID6 156,992; 2 stripes with log commits 113,496. With 'full' (also a present device skipped as already WRITE_FAILED whose last superblock has the current generation): 0 in all of these.)

Scenario: The failed device is admitted outside write_all_supers: T-fua on the write-intent log block written at every mark, T-admin, T-repair or T-log. Then a log commit and a crash.

Trace: d0 starts failing writes || write s0.d0=2 T-fua(log) fails d0 ; LOG commit ok || CRASH ; mount (tie: sb of {d0}) => LOG ROOT MISSING; d0 is also not listed and so trusted. d1 and P hold sb(G, list{d0}, log root); d0 holds sb(G, list{}, no log root). T-admin variant: trigger fail d0 || write s0.d0=2 ; LOG commit ok || CRASH ; mount (tie: sb of {d0}).

Kernel: Design only (stage 1 is not implemented). ERRATA 6's parenthetical ('FUA failed, or admitted during that write_all_supers') omits 'skipped as already WRITE_FAILED', which design-model-result.md change 1 includes. X-1 above is the same mechanism in today's kernel.

### C (ERRATA 1 as written): LOST_ACKED (unreachable; loud EIO) (Loud loss of acknowledged data. 1,392 of 9,033 states with no crash, against 0 when only possibly-torn records are classified. With 1 crash: 18,872 against 9,336. RAID6: 0.)

Scenario: A persistently failing disk that is later failed; no crash at all, only a clean unmount and mount (E_persist_C_e1all_crash0, 2 stripes, depth 10).

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || trigger fail d0 || unmount ; mount => the record was kept as vague sticky by forget (§2.4); ERRATA 1 applies §3.5 to it; RAID5 with a nodatasum sector gives 'suspect'. s0.d0 reads EIO forever while P holds 2; replace writes zeros.

Kernel: Not in stage-0. The kernel classifies only when the record is marked possibly torn: 'if (absent && torn …)' at scrub.c:5640, with the comment at 5637 that classifying a plain failed write 'would read as EIO … for nothing'. The testing-only knob raid56_wf_all_records_torn (raid56-wib.c:185, 259) restores the ERRATA 1 behaviour. Stage 1 must keep the torn-only rule.

### C (pending window, §2.4 gate): LOST_ACKED (unreachable; loud EIO) (Loud. P_persist_Cpend: 16,884 unreachable against 11,472 with the fix; with T-fua 10,144 against 7,472; silent 0 either way.)

Scenario: A crash between the durability commit (or any commit while the failure is pending) and the next log write that drops the gate's residues (residue_demo.py, a replay through the model's own transitions).

Trace: d0 starts failing writes || write s0.d1=2 (a HEALTHY column) T-fua(log) fails d0 ; TXN commit (d0 durable) ok || CRASH (idle) ; mount => the finished write's in-flight mark was kept on disk by 'flushed &= !gate'; d0 is listed, so the stripe is taken for torn with an absent nodatasum column: s0 is suspect, and s0.d0 (acknowledged 1, never written, P=(1,2) consistent) reads EIO forever. With a 'finished' flag on gated entries (--residue-smart): readable.

Kernel: Design only (the gate is §2.4 of the design).

### CTM (T-missing) with ERRATA-6-full and mount-commit, without a generation skip: SILENT_WRONG (data path) (Silent. 680 of 573,413 states (depth 6). With --gen-skip (the degraded read-write mount commits at G+2): 0 (476,801 states). Inferred, not modelled: T-missing's witness rule (sb_bound = last committed generation = G) would also take d0 (generation G+1) for a replacement, so sb_bound must be G+1.)

Scenario: An orphan superblock: a commit that crashed after landing only on d0, with d0 missing at the next mount; T-missing fails d0; d0 comes back.

Trace: txn commit ; TXN commit sb landed {d0} ; CRASH ; mount without d0 T-missing fails d0 (mount commit) || write s0.d0=2 ; TXN commit sb landed {} ; CRASH ; mount with {d0} back (tie: sb of {d0}) (d0 not listed: trusted) || d1 starts failing writes || txn commit ; TXN commit T-fua(sb) fails {d1} => d0's orphan G+1 superblock (no list) ties with the session's G+1 (list{d0}). d0 is trusted with 1 while P=(2,1). A rebuild of d1 returns garbage with no error.

Kernel: Design only (T-missing is optional in stage 2). The underlying generation collision is the upstream X-2 above.

### C and CTM (pending) with F1 full, f1-missing and gen-skip, without a mount commit: SILENT_WRONG (data path) (Silent. 1,541 of 1,016,141 states (CTM, depth 6); 306 at depth 5. With --mount-commit (commit the picked superblock's list to every present device before forget and recovery): 0.)

Scenario: A torn superblock write leaves a tie between superblocks with and without the list. The mount acts on its pick (forget, recovery drops records) before making that pick durable; a later mount picks the other superblock.

Trace: P starts failing writes || write s0.d0=2 T-fua(log) fails P ; LOG commit sb landed {d0} ; CRASH ; mount (tie: sb of {d0}: P FAILED, recovery drops s0's record) || txn commit ; TXN commit sb landed {} ; CRASH ; mount (tie: sb of {d1,P}) (P not listed: trusted) || d1 detaches => s0.d1 is rebuilt from d0=2 and P's stale (1,1): garbage, no error

Kernel: Design only (§1.6 mount order).

### C (T-fua as specified in §1.2): REFUSED availability / permanent degradation (Availability plus exposure. C: 90 of 160 crash points lose acknowledged data (22 gone) after a glitch A absorbs with 6 refusals and 0 losses (A+NI). Exhaustive: FAILDEV 5,911; LOST gone 1,840 against A's 0. This contradicts §1.2's 'transient: records only, no transition' row whenever the fault covers the log region.)

Scenario: A 6-write error_writes glitch on d0 that heals (sched transient_d0; P_transient_Cpend_tfua).

Trace: d0 starts failing writes || write s0.d0=… T-fua(log) fails d0 (the first mark's log-block write) … d0 heals => d0 stays FAILED for good in stage 1; every later crash point runs degraded

Kernel: Design only.

### C (stage-1 read rule: the failed disk is never a rebuild source): LOST_ACKED (unreachable; loud) (Loud. Unreachable 1,832 against 1,000 with verified sources; needless EIO 8,216 against 4,764. A keeps d0 as a readable member and reads d1's data.)

Scenario: RAID5 checksummed data: d0 failed, then d1 lost (N_r5second_csum_C_nosrc, depth 7).

Trace: d0 starts failing writes || trigger fail d0 || d1 detaches (beyond tolerance: read-only) => every checksummed cell of d1 reads EIO forever, although d0 holds all of them and they verify (nothing was written after the failure); no admin path back (read-only)

Kernel: Design only (§2.3, §3.4, Risk 13).

### C (all variants, including all fixes): LOST_ACKED (gone) / RO (Loud loss. csum: 1,296 gone (P_persist_Cpend_csum). Metadata: 2,184 gone and 888 RO, against A's 0 gone and 12,004 RO. Schedules: 20 to 24 of 88 to 168 crash points (csum), all gone.)

Scenario: C-1 and C-5 re-confirmed with the non-atomic trigger: a sub-stripe write while degraded, then a crash.

Trace: d0 starts failing writes || write s0.d0=2 T-fua(log) fails d0 ; TXN commit (d0 durable) ok || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount => d0's acknowledged 2 was only in P, and P is torn

Kernel: Design only. A refuses the second write (raid56.c phase-A refusal).

### C: LOST_ACKED (gone and unreachable) (Loud. nodatasum: C 1,912 gone and 59,844 unreachable, against A 0 and 14,568. csum: C 1,696 and 176, against A 0 and 0.)

Scenario: RAID6 with two devices failed (R_two_failed_*, depth 7).

Trace: d0 starts failing writes || P starts failing writes || write s0.d0=2 ok (write to {d0,P} failed) || trigger fail d0 || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount => s0.d0 is gone (the stripe is RAID0 once two members are failed)

Kernel: Design only.

### C + strict_degraded + narrow in-flight (the earlier refinement): LOST_ACKED (gone) (Loud. 12,264 gone. With the added sd_csum check: 0 gone, 54,580 refused writes, against A+NI's 33,716. RESULTS.md's 'C+SD+NI loss-free' holds for nodatasum only.)

Scenario: Checksummed data: the refinement lets an RMW rebuild a checksummed failed-disk column whose on-disk content no longer verifies (V_persist_csum_*, depth 9).

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || trigger fail d0 || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount => gone

Kernel: n/a (a model refinement, not kernel code).

### C with every fix but without T-missing (RAID6); A with every fix (RAID6): SILENT_WRONG (model-dependent; not claimed) (C: 506 of 784,645 states (depth 5 T); 8 at depth 4. CTM: 0. A: 52, reached only through the model's replace, which excludes the source parity.)

Scenario: Three faults: a torn write lands on d0, d0 is missing at the mount, recovery decides its column from P and Q, d0 returns trusted, then two more members fail or are replaced.

Trace: C: write s0.d0=2 ; phaseB landed {d0} ; CRASH ; mount without d0 || d1 starts failing writes || P starts failing writes || unmount ; mount with {d0} back (d1 not listed: trusted) => d1 is rebuilt from d0=2 and Q=(1,1): garbage

Kernel: Probably not. The kernel keeps a sticky record for a later pass after BTRFS_WIB_STRIPE_DECIDED (raid56-wib.c:7572-7580, btrfs_wib_parity_unwritten at 4554-4595); the model drops it. A kernel replace would also cross-check with the readable source parity. Listed so the reader can weigh it; not a finding.

## Sensitivity

**Regression.** `regress.log`: with every new switch off, c_model.py (final md5 d41b2a2f) gives the same counters as policy_model.py (3ed8fa4f) in 8 of 8 configurations: C persistent_full, C missing_return_full, CTM missing_return, A transient, CUR replay_full, C flush, C RAID6, CB meta.

**Negative controls.** Each removes one protection and must show its failure class. All bite except one, as expected:

| Control | Result |
|---|---|
| Retire-early (C with superblocks but no pending window: forget at the trigger) | SILENT 6,732 (S_control_Cretire_early); 10,378 (P_persist_C_retire_early_ctl). With the pending window: 0. |
| c_no_gate (drops allowed while pending), with a detach | SILENT 2,022 (M_ctl_detach_Cpend_nogate, final model), against 0 (M_detach_Cpend) |
| ERRATA 6 rule 'errata' against 'full' | 26,944 to 156,992 against 0, in eight families |
| A with F1 'none' against 'any' | 59,136 against 0 (persistent); 115,422 against 0 (transient) |
| no_tombstone | SILENT 2,710 against 0 |
| --e1all | LOST 1,392 with no crash, against 0 |
| --residue-smart | halves LOST(unreach): 16,884 to 11,472, and 10,144 to 7,472 |
| The three missing-device fixes, each removed alone (group M) | without f1-missing, gen-skip and mount-commit: A 71,926 and CTM 36,204; without gen-skip only: 680; without mount-commit only: 1,541; with all three: 0 |
| sd_csum | 12,264 gone to 0 |
| Original model's c_trust_unverified | 171,592 silent (unchanged model code) |
| P_missing_CTMpend_ctl_retire | 0, which is **not** a control: under CTM a missing device is admitted before any record names it, so nothing can be forgotten early |

**Checks against model bugs.**
- **Log-sequence clamp.** An early clamp of the log-commit sequence produced 8 false "data-path" silent states. The clamp was removed and the run re-checked: every silent state is then log-root.
- **Read-only mount forget.** A read-only mount's in-memory forget leaked into the next mount (a false silent in an early D run). Fixed and re-run as groups M and D.
- **Residues at mount.** Loaded residues are now converted to possibly-torn before any commit at mount.
- **Two apparent RAID6 silent classes were checked by step-by-step replay** (`replay.py`) and rejected as model-dependent (last counterexample).
- **Schedules.** All five schedule tables are identical under the earlier model (out_v2/) and the final one.

## Caveats

**REPORT.md was not written.** The environment refuses report files from subagents ("return findings as text"). The full report is this output; the raw per-run results are in attack_C/out/*.txt (RESULT line, CLASS traces, first traces), summary_table.txt and sched_*.txt.

**Model versions.**
- Groups E, N, R, V, P, G and S (the S re-runs) used c_model md5 0bb0e408.
- Groups M, D and jobs9 used the final version (md5 d41b2a2f). It adds f1-missing, gen-skip, mount-commit, orphan detection, the read-only-mount fix and residues loaded as torn.
- The two versions differ only for superblock runs that mount read-only with a listed device, or that load residues. The affected detach/control runs were re-run as group M with the same verdicts. The schedules are identical under both.
- out_v1/ holds S runs from an earlier residue semantics (re-run). out_badflags/ holds two runs launched without --f1 any (discarded).

**Abstraction.**
- **Geometry and concurrency.**
  - One sector per column, no parity rotation, 1-2 full stripes (4 in the schedules).
  - One operation at a time. Admission mid-operation is only at the mark (T-fua), the design's §2.1 re-sample point.
  - Replace is atomic, apart from the returning old disk. Mount recovery is atomic per stripe (with --recov-crash: a crash after any prefix).
- **Superblocks.**
  - Generations are relative and clamped at -3.
  - Mirror superblock copies are not modelled.
  - The witness rule (sb_bound) is not modelled; only the tombstone refusal is. The sb_bound claim in the orphan finding is inferred.
  - Ties are adversarial. In the kernel the pick depends on device scan order, so counts bound how often it can happen, not how often it does.
- **Log-root loss and orphan mounts.** Both are judged as terminal SILENT states. The model does not simulate the reverted tree contents: the orphan-lineage effect on tree blocks (same-generation overwrite) is argued from the code, not simulated.
- **The residue window** runs to the next log write (a mark or a commit). The design's devstate worker calls persist_now soon after durability, so the real window is shorter, but a crash in it has the modelled effect.

**Not covered.**
- Stage 2: readmit, the written-since-failure map, R7-R9.
- Device remove; mirrored profiles and T-mirror; transid-0 metadata readers; checksum collisions.
- The design-model attack (wf_model) covered readmit, the DSB copy rule and replace resume.

**Search bounds.**
- Depths 4-12. Several runs are marked T (memory guard, 2.8 GB); their counts are exact up to that depth.
- LOST and STUCK are judged by an admin closure of ≤3 actions.
- Counts are distinct states and grow differently per policy: compare zero against non-zero, and the traces.
- The availability schedules are single deterministic runs. They count crash points, not probabilities.

**Model-dependent results.** The last counterexample (RAID6 triple fault, C 506 / A 52) depends on the model dropping the recovery's DECIDED record and on its replace excluding the source parity. It is not claimed.

**Kernel reproduction.** None of the kernel-applicable findings (X-1 log-root tie, X-2 orphan superblock, X-3 replaced disk returns) was reproduced on UML. Kernel lines were read at BTRFS-s0 stage0-wip 2a1e2379e4. A suggested UML arm for X-1 is in its counterexample.

**Fixes not implemented.** The fixes named here were checked only in the model and exist in no kernel: ERRATA 6 'full'/'any' also covering missing devices, gen-skip, mount-commit, the residue 'finished' flag, sd_csum, and §3.5 on torn records only.

## Model files

- <scratch>/attack_C/c_model.py
- <scratch>/attack_C/c_model.md5
- <scratch>/attack_C/policy_model.orig.py
- <scratch>/attack_C/c_model.v1.py
- <scratch>/attack_C/c_model.v2.py
- <scratch>/attack_C/regress.sh
- <scratch>/attack_C/regress.log
- <scratch>/attack_C/run_jobs.sh
- <scratch>/attack_C/jobs1.txt
- <scratch>/attack_C/jobs2.txt
- <scratch>/attack_C/jobs3.txt
- <scratch>/attack_C/jobs4a.txt
- <scratch>/attack_C/jobs8.txt
- <scratch>/attack_C/jobs9.txt
- <scratch>/attack_C/summ.py
- <scratch>/attack_C/summary_table.txt
- <scratch>/attack_C/sched.py
- <scratch>/attack_C/sched_r5_nodatasum.txt
- <scratch>/attack_C/sched_r5_csum.txt
- <scratch>/attack_C/sched_r6_nodatasum.txt
- <scratch>/attack_C/sched_r6_csum.txt
- <scratch>/attack_C/sched_r5_s3c1_nodatasum.txt
- <scratch>/attack_C/residue_demo.py
- <scratch>/attack_C/residue_demo.out
- <scratch>/attack_C/replay.py
- <scratch>/attack_C/sched_dbg.py
- <scratch>/attack_C/sched_dbg2.py
- <scratch>/attack_C/out/
- <scratch>/attack_C/out_v1/
- <scratch>/attack_C/out_v2/
- <scratch>/attack_C/out_badflags/
