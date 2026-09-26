# synthesis

*(This is the full text for COMPARISON.md. The harness does not let subagents write report files, and every other agent in this run was refused, so it is not on disk. The parent can save it verbatim.)*

# RAID5/6 write-failure policies: which one keeps your data

## 1. The short answer

Your rule is "I would rather the system act up than lose data." Measured against that rule:

- **Today's kernel (CUR) breaks it.**
  - When its log is full, it throws away records that say "this disk's copy of this stripe is stale". It does this in three situations:
    - a device is missing anywhere in the filesystem;
    - a tree-log replay is running at mount;
    - with all disks present, the record is "torn-only".
  - It also acknowledges some flush failures that it could not record.
  - Afterwards, a read of nodatasum data can return old or garbage data **with no error**.
  - The model found 11 such classes (CUR-1 to CUR-11). Each one traces to kernel code, and the fidelity critic confirmed the original five against the code.
  - An alert is raised when the record is dropped, but it lives only in memory and is gone after a remount.
- **Strict refusal (A) never returned wrong data because of the log's own rules.** That holds for every run of A across the seven model efforts.
  - Its price: refused writes (EIO), read-only for metadata, a mount that can fail on a full log, and a stuck state when there is no spare disk.
  - Its biggest loss is acknowledged data that stays on disk but reads EIO for good (A-1). A small reordering in the write path (NI) removes it. NI is not implemented.
- **The kernel cannot give you A today.**
  - The two switches that turn CUR into A exist only in debug builds.
  - Even with both set, several kernel paths still drop a record or never write one: K1-K5, CUR-4 and CUR-11.
  - Each has a fix of limited scope (section 7).
- **A2, your proposed middle ground, is not safe as written.**
  - It had four silent holes, all in the gap between "drop the record" and "taint the disk".
  - With about ten rules added (section 5), it is never silent in the model. It keeps writing where A wedges, and it is never stuck where A is not.
  - With a whole-disk taint it loses more data than A (loudly) after a second disk problem. A per-region taint with a resync removes that; this is the design's stage-2 map.
- **B (degrade per stripe) is dominated.** One failing disk plus one crash loses acknowledged data that A keeps, in 5.7% of all fault schedules.
- **C (md-style failed disk, stage 1) does not make data safer than A.**
  - Running degraded on the failed disk is exactly B's hole: a crash during a later partial write destroys the failed disk's acknowledged data. The loss is loud, not silent.
  - Before its trigger, the design as written behaves as B.
  - The spec has four silent holes, all fixable.
  - Built with the fixes, C is a useful **operational** state ("this disk is failed, replace it").
  - To also meet your rule, C must refuse the partial writes that would open the hole. It then loses nothing, but it is not more available than A with NI.
- **Three silent holes are not in the log at all.** They are upstream btrfs behaviour, they hit every policy, and checksums do not help:
  - X-1: fsync'd data is lost when a mount picks a failing disk's superblock;
  - X-2: an orphan superblock is mounted after a degraded session;
  - X-3: an old disk comes back in place of its replacement.
- **Checksums matter.** With checksummed data, every silent class of the log turns into an EIO, so wrong data is never served. Data can still be lost, but you are told.

Nothing here was run on a real kernel.

## 2. How to read the numbers

- **What the model does.** It tries every sequence of writes, faults, crashes, mounts and admin actions up to a fixed length, on a tiny array: 3-4 disks, 2-3 stripes, one sector per disk per stripe, and a log of 1-4 records.
- **What is counted.** Counts are distinct states the search reached; refusals are counted as distinct transitions.
  - Policies reach very different numbers of states, so **compare zero against non-zero, not the sizes**.
  - Where the fidelity critic computed it, I also give the **refused share**: refused writes divided by attempted writes. That share is comparable across policies.
- **Terms used below:**
  - **Silent:** a read returns something other than the last acknowledged value, with no error.
  - **Gone:** the acknowledged value is on no disk any more.
  - **Unreadable:** the value is still on disk, usually in parity, but no sequence of up to 3 admin actions gets it back. The actions are heal, replace, scrub, remount and ack.
  - **Stuck:** no admin action restores writing without losing acknowledged data.
  - **Needless EIO:** a read fails although the value could be recovered.
- **Defaults:** RAID5 and nodatasum data unless stated otherwise.
- **Corrected values.** A number marked † is the fidelity critic's corrected value (section 8); the first number is the original run. The ranking does not change.
- **Kernel references** are from code reading at stage0-wip 2a1e2379e4, which I checked is still the branch head.
- **Scale:** about 1,250 exhaustive runs by seven agents, plus 38.7 million fixed fault schedules for B and five availability schedules for C.

## 3. Comparison table

Each cell gives the finding and the state count behind it.

| Policy | Silent wrong reads | Acknowledged data gone from every disk | Acknowledged data on disk but unreadable | Refused writes / read-only | Stuck | Needless EIO |
|---|---|---|---|---|---|---|
| **OLD** (upstream, no log) | **Everywhere.** 310k states (persistent), 258k (flush), 35k (the classic write hole). Checksummed: 0 | 206k (persistent); checksummed 18k | 65k | Never refuses | 0 | 0 |
| **CUR** (kernel today) | **11 classes.** 38,464 (34,124†) degraded eviction then the disk returns; 24,508 unnamed flush; 48,260 flush dropped outright; 1,520 (512†) replay; 8,896 unrelated disk missing; 96k (87k†, run stopped at depth 6) RAID6. 0 with every disk present and no replay. Checksummed: 0 | 32k (28k†) in missing_return_full, where A has 1,312 (768†) | 42,356 (14,102†) in persistent, the same as A | The same as A with every disk present: persistent 81k (63k†), 16.2%† of writes. While degraded it keeps writing (24 of 24 in a test run). The mount fails at replay in 384 cases | 0 (the 32 RAID6 states were a model artifact†) | 42,356 (persistent); 16,896 (missing_return_full) |
| **A** (strict refusal, as modelled) | **None** from the log's rules | Only the degraded write hole (a crash while a disk is missing): crash_loss 14,200 (9,272†); missing_return_full 1,312 (768†); 0 with every disk present | A-1: 42,356 (14,102†) in persistent; 0 with NI | Persistent 81k (63k†), 16.2%† of writes. Degraded with a full log: 56.1%† (3 of 24 writes accepted in a test run). Metadata read-only 11k; the mount fails at replay 1,920 (1,536†) | 2,432 (1,536†): a failing disk and no spare | 42,356 (persistent) |
| **A + NI** (not implemented) | None | As A | **0** in persistent | 44,724 (39k†); 10.9%† of writes | Not run | 0 (persistent) |
| **A2 as specified** | 0 in the shared matrix, but the attack found 4 holes: 7,650 (a replace's own zeros), 3,456 (taint not yet on disk), 12,030 (resumed scrub), plus 1,476 over A when an old disk returns after a replace | missing_return_full 7,120 (6,576†); 3,240 after two absences in turn (A: 0) | 8,824 after two absences (A: 0); 6,776 when a second disk dies after the return (A: 0); 2,112 lost only because the whole disk is tainted | Degraded: keeps writing (24 of 24; A: 3 of 24). After the return: 10 of 24 (A: 17 of 24), unless repair may write onto a tainted disk. At replay the mount still fails until the failing disk is pulled | 0 (A: 2,432) | 22,608 (missing_return_full) |
| **A2 with all fixes** | **0** in all 22 families run | 0 after two absences, and 0 when a second disk dies after the return. missing_return_full 8,128 (A: 1,312), because A2 keeps writing while degraded and more writes meet the degraded write hole | 0 after two absences; missing_return_full 17,120 (A: 2,912) | 0 refused after two absences (A: 8,288) and after the return (A: 10,512). Still refuses as A does when verdicts fill the log after a crash while degraded, or on RAID6 with a present failing disk | 0 where A is not stuck | Not reported |
| **B** (per-stripe degrade) | 0, but only because of the torn-read refusal and the mount's verdict. A literal "never refuse" B: 150,228 of 544,942 schedules silent | persistent 19k. **One failing disk plus one crash: 212,862 of 3,717,550 schedules (A: 0).** Metadata: 28,858 schedules (A: 0 lost, read-only instead) | persistent 45k | persistent 63k | 2,432 with no spare | 48,572 |
| **C as designed** (§2.1: phase A never refuses before the trigger; ERRATA 2 and 6 as written) | **Yes.** 24,320 (19k†) untainted drop of a missing disk's records (ERRATA 2); 26,944-156,992 fsync'd data lost with the log root (ERRATA 6 gap); 680 and 1,541 from the superblock pick | CB persistent 49k; C 19k (16,144†); the same 212,862 schedules as B; metadata 2,032-2,184 lost where A goes read-only | persistent 129k (63k†) | 23.5%† of writes in persistent (A: 16.2%†). In a "failed for good" schedule only 1-2 of 24 writes are refused, but 82-90 crash points lose data | 1,788 (checksummed): read-only for good after a second disk loss | 134,450 (persistent) |
| **C, spec fixed + T-missing** (phase A refuses until the trigger) | **0** in every run | Still loses: persistent 16k†, metadata 2,184, RAID6 with two disks failed 1,912 (A: 0) | persistent 63k†; RAID6 with two disks failed 59,844 (A: 14,568) | Few refused, as above | Read-only for good after a second loss | 1,832 against 1,000 when the failed disk is never used as a rebuild source |
| **C strict** (C + SD + NI + the checksum case) | 0 | **0** | **0** (persistent) | 54,580 against A+NI's 33,716 transitions; 16.1%† against 10.9%† of writes; 12 of 24 in the "failed for good" schedule (A: 18 of 24) | Not reported | Not reported |

**Per-family counts from the shared matrix.** These are the original runs, nodatasum. Each cell reads SILENT / GONE / UNREADABLE / STUCK / REF_WRITE / RO; k = thousands; T6 means the run stopped on memory at depth 6.

| Family | OLD | A | CUR | A2 | B | C | CB | CTM |
|---|---|---|---|---|---|---|---|---|
| persistent (d10) | 310k/206k/65k/0/0/0 | 0/0/42k/0/81k/0 | = A | = A | 0/19k/45k/0/63k/0 | 0/19k/129k/0/180k/0 | 0/49k/134k/0/174k/0 | = C |
| flush_unnamed (d9) | 36k/23k/7384/0/0/0 | 0/0/0/0/4472/4472 | **25k**/16k/8494/0/0/0 | = A | = A | 0/1472/6008/0/6880/0 | = C | = C |
| crash_loss (d10) | 35k/35k/0/0/0/0 | 0/14k/27k/0/47k/0 | = A | = A | = A | = A | = A | = A |
| missing_return_full (d10, cap 1) | 35k/25k/9680/0/0/0 | 0/1312/2912/0/28k/0 | **38k**/32k/21k/0/52k/0 | 0/7120/22k/0/98k/0 | = A | **24k**/21k/19k/0/72k/0 | = C | 0/3312/9936/0/40k/0 |
| cap2_missing_return (d7) | 6032/2908/3124/0/0/0 | 0/0/0/0/8028/0 | **1788**/576/1212/0/0/0 | 0/0/0/0/288/0 | = A | = CUR | = CUR | 0/0/0/0/0/0 |
| replay_full (d9, cap 1) | 16k/11k/256/0/0/0 | 0/0/1536/0/14k/1920 | **1520**/756/512/0/18k/384 | 0/0/512/0/14k/1920 | 0/256/1536/0/12k/1536 | 0/760/4076/0/16k/384 | 0/1272/4080/0/16k/0 | = C |
| nospare_persistent_full (d10) | 16k/11k/256/0/0/0 | 0/0/768/**2432**/13k/0 | 0/0/768/0/13k/0 | = CUR | 0/256/768/2432/12k/0 | 0/768/4736/0/18k/0 | 0/1280/4736/0/18k/0 | = C |
| raid6_bad_missing_full (d7) | 443k/199k/113k/0/0/0 T6 | 0/5096/39k/0/274k/0 | **96k**/33k/130k/32/146k/0 T6 | 0/8488/84k/0/377k/0 | 0/13k/47k/0/294k/0 | 0/22k/164k/0/236k/0 T6 | 0/31k/168k/0/228k/0 T6 | 0/34k/201k/0/372k/0 |
| meta_persistent_full (checksummed metadata, d9) | 0/2032/0/0/984/984 | 0/0/0/0/11k/11k | = A | = A | 0/896/0/0/10k/10k | 0/**2032**/0/0/5108/5108 | 0/2544/0/0/3720/3720 | = C |

## 4. The kernel today (CUR): what conflicts with your rule

Code references are in raid56-wib.c unless another file is named.

| # | What CUR does | Where | What goes wrong | Conflicts with your rule? | Fix |
|---|---|---|---|---|---|
| 1 | While **any** device is missing, a full log spends records naming stale disks, then the mount's verdicts. "Any" includes a disk that holds nothing of this RAID5 chunk, and a hot-unplugged one. The stale disk may even be a present, failing one. | `wib_may_evict_naming()` 716-726 (tests `missing_devices` at 724); `wib_evict_sticky()` 801, passes at 847 | CUR-1, CUR-2, CUR-5, CUR-6, CUR-7, CUR-8 | Yes | A2's drop rule (section 5) |
| 2 | During tree-log replay at mount, the same eviction happens with every disk present. | `BTRFS_FS_LOG_RECOVERING`, set and cleared at tree-log.c:7755 and 7908; tested at 725 | CUR-3, plus D6 when a disk is also missing | Yes, but only if metadata (or space_cache=v1) is on RAID5/6 | Refuse. The mount fails until the failing disk is pulled, then A2's drop and taint lets it through |
| 3 | With every disk present, it spends "torn-only" records as a last resort. These come from a flush it could not name, or a stripe the mount could not read. | `wib_entry_torn_only()` 775; kept_torn 784; passes 856-858; fallback 3589-3594; `btrfs_wib_try_mark()` 1196 | CUR-9, CUR-10: an EIO becomes garbage or an older version | Yes | Never spend them (A's `raid56_wf_torn_unevictable=1`) |
| 4 | It acknowledges a flush loss it could not name. | `wib_readd_dropped()`: UNNAMED plan 3014; `EV_LOG_UNFLUSHED` 3196; message 3211 | CUR-4 | Yes, even with A's knobs | Fail the commit (read-only) |
| 5 | It drops a flush loss outright (the LOSE plan). No knob controls this. | 3016, 3026, 3049; `EV_DROPPED` 3198 | CUR-11 | Yes, even with A's knobs | Fail the commit (read-only) |
| 6 | The first record that names a disk halves the capacity from 165 entries to 82. If nothing can be evicted, the commit logs "block full, keeping the previous one" and returns success. | `wib_live_max()` 527-537; `wib_enforce_capacity_locked()` 941; `btrfs_wib_commit()` 5757-5776 (checked) | K3: after a crash the names that were only in memory are lost, and parity is rebuilt from the stale disk | Yes. This is the worst case under A's knobs | R1: never admit more than 82 entries; R2: a commit that cannot write the log aborts |
| 7 | Mount recovery drops a record it cannot fit, then mounts read-write. | `btrfs_wib_add_sticky()` 3763-3782; 7335-7342 | K2 | Yes | Stay read-only, or use A2's drop and taint |
| 8 | A failed write's record names nothing for a moment, and a concurrent write may evict it in that window. | raid56.c:5076, then 5128 | K1 (a race) | Yes | Name the disk under the same lock |
| 9 | A user scrub reads the committed tree. It retires a record of the running transaction after rebuilding parity from the stale disk. | scrub.c:3843 and 4004; no generation check at 4793 | K4, with no alert | Yes | Compare generations, or commit before scrubbing as replace does |
| 10 | Full-stripe writes are not logged, so a lost flush on one is recorded nowhere. | raid56.c:4926-4940; `btrfs_wib_note_written()` 2203-2262 | K5, with no raid56 alert | Yes | Track and name them, or abort the commit |
| 11 | It marks the whole stripe "in flight" before repairing a stale disk. | raid56.c:4939, before 4977 | A-1: acknowledged data reads EIO for good although parity holds it | No: the failure is loud. It is a real cost | NI |
| 12 | The alert is kept in memory only. Its advice ("fix or replace the failing device", "run scrub") makes CUR-1 permanent. | raid56-wib.h:813; text at 6138-6142 | The admin cannot act in time | Weakens every alert | Persist the alert. Advise replacing the missing devid with a new disk |
| 13 | The switches that give strict refusal exist only in debug builds. | 586; constant false at 654-658 (checked) | A production kernel cannot choose A | Yes | Make refusal the default |
| 14 | A device scan of a returning disk clears MISSING although the disk is not opened. | volumes.c:959-962 | CUR refuses until a remount (safe, but surprising) | No (availability only) | Define "missing" as `bdev == NULL` |

**What CUR already does right:**
- With every disk present and no replay, CUR behaves exactly as A: 0 silent states in persistent_full at depth 12, and in transient_full and flush_full at depth 11.
- Refusing a failed phase-A repair (raid56.c:4743) is what keeps B's loss out of today's kernel.
- Checksummed data is never silent in the log's classes.

**Upstream holes that hit every policy (not the log's):** X-1, X-2 and X-3 (section 7).

## 5. Is A2 sound?

**Not as specified.** In the model it becomes sound with the rules below. Each rule is backed by a run that goes silent without it and shows 0 silent states with it.

**Holes found, and the rule that closes each:**

| Hole | Silent states | Rule that closes it (0 in the model) |
|---|---|---|
| A replace of a missing disk writes zeros where it cannot rebuild, and records that. The record names only the missing source, so it looks droppable. The finished replace then clears the taint, and the zeros are served as data. | 7,650 | Never drop a running replace's own marks. Keep CUR's `replace_marks_lost` rule: the replace fails instead. A replace may clear only a taint older than itself. |
| The block without the record reaches disk before the taint does, and a crash in between trusts the stale disk. | 3,456 (value model); 91,111 RAID5 / 589,033 RAID6 bad mounts (persistence model) | Put the taint in the header of every log block written while it holds (0 violations). The alternative is the superblock with zero write errors, which also gives 0 but allows no drop while any disk fails writes. |
| The taint is kept in a superblock under upstream's "one copy is enough" rule. | 6,246 / 42,651 | As above. |
| A resumed scrub (`btrfs scrub resume`) finishes a pass that began before the taint, and clears it. | 12,030 whole-disk; 16,688 per-region | A pass clears only taints older than its start, and must cover the whole disk within that pass. |
| The old disk returns in place of its replacement. This also happens under A, CUR and OLD. | 1,476 more than A | Keep a tombstone for every replaced devid whose superblock was not scratched. Key the taint by devid plus uuid. |

**Rules for the implementation** (each is confirmed necessary by a negative control):
- **The drop test.**
  - Check every stale bit of the 64-block entry, mapped through the chunk map to a device with `bdev == NULL`. Checking only one stripe of the entry gives 3,590 silent states.
  - Refuse the drop if the entry is torn, a verdict, a bitmap, or owned by a replace.
- **"Missing" means `bdev == NULL`,** not the MISSING flag (row 14 of section 4).
- **Honour the taint at five sites:**
  - bio.c:543;
  - raid56.c:2060;
  - scrub.c:3357, 5385 and 5122;
  - scrub.c:931 and 3371;
  - scrub.c:2716.

  Missing any one site gives 5,428 to 25,392 silent states. The lock-free `nr_stale` fast paths (3843 and 4712) must also count taints.
- **Taint means "don't trust reads", not "don't write".** Repair and mount recovery must still write rebuilt data onto a tainted disk. Otherwise A2 refuses more than A after the return: 10 of 24 writes accepted against A's 17.
- **Set an INCOMPAT bit while any disk is tainted.** The log is COMPAT_RO, so an older kernel mounting read-only ignores both records and taint. This is argued, not modelled.

**What A2 buys over A:**
- It removes CUR-1, -2, -3, -5, -6, -7 and -8 without A's wedges.
- It keeps writing while degraded: 24 of 24 writes against A's 3 of 24. In the kernel, A refuses after about 82 scattered 4 MiB regions (roughly 328 MiB).
- It is never stuck without a spare disk: 0 against A's 2,432.
- It has a way past a failed replay mount: pull the failing disk.

**What it costs against A:**
- **A whole-disk taint loses acknowledged data loudly where A loses none:** 12,064 states after two absences in turn, and 6,776 when a second disk dies after the return. The dropped record was the only thing that told the return mount which stripe to repair.
  - Fix: a per-region taint, cleared when the region is rewritten, plus a resync at the return mount. That gives 0 lost and 0 refused in both cases. It is the design's stage-2 written-since-failure map applied to missing disks.
- **A2 still refuses exactly as A does** when the mount's verdicts fill the log after a crash while degraded, and on RAID6 with a present failing disk.
- **It keeps A's other costs:**
  - A-1, unless NI is added;
  - the degraded write hole, with more exposure because A2 keeps writing while degraded;
  - a failed mount at replay until the failing disk is pulled.

**What A2 does not fix:**
- CUR-4 and CUR-11: the commit must abort instead.
- CUR-9 and CUR-10: keep A's rule of never spending torn records.
- K1-K5.
- X-1 to X-3.

## 6. How stage 1 (C) changes the picture

- **Before the trigger, the design as written is B.** §2.1 says "phase A never refuses".
  - One failing disk plus one crash loses acknowledged data in 212,862 of 3.7 million schedules.
  - Metadata is lost in 28,858 of 544,942 schedules, where A goes read-only.
  - **Ship stage 1 with `raid56_wf_phase_a_refuses=1` as the default.**
  - The cost is known from the design's own S1-01 measurement: RAID5/6 metadata goes read-only 0.01-27 s after a disk starts failing writes (8 of 8 arms).
- **After the trigger, the array runs degraded on the failed disk.**
  - The failed disk's data now lives only in parity. Every later partial write to that stripe rewrites the parity, and a crash during that write loses the data.
    - C-1: 16,144† states gone in persistent; A: 0.
    - Metadata: 2,184 states lost, where A goes read-only.
    - RAID6 with two disks failed: 1,912 gone; A: 0.
  - Every crash while degraded makes the failed column's nodatasum sectors unreadable (C-2: 63k†).
- **A second disk loss leaves RAID5 read-only for good** (C-4: 1,788 stuck states).
  - A keeps the failing disk as a readable member and can replace the missing one.
  - The design's rule "a failed disk is never a rebuild source" makes this worse even for checksummed data: 1,832 unreadable states against 1,000. Allow checksum-verified reads from the failed disk as a source.
- **Silent holes in the spec, each with a fix checked at 0:**
  1. ERRATA 6 misses disks marked failed outside the superblock write (T-fua on the log block, T-admin, T-repair, T-log). The mount then drops fsync'd writes: 26,944 to 156,992 silent states.
     - Fix: also cover "skipped as already failed", as change 1 of design-model-result.md does, and extend the rule to missing disks.
  2. ERRATA 2 keeps CUR's untainted drop for a missing disk: 24,320 (19k†) silent states, and 11,344 even with SD+NI.
     - Fix: T-missing (CTM) or A2's taint.
  3. T-missing is fooled by an orphan superblock: 680 states.
     - Fix: a degraded read-write mount skips a generation, and `sb_bound` becomes G+1.
  4. A mount acts on a superblock it picked out of a tie before writing that pick back: 1,541 states.
     - Fix: commit the pick at mount, before recovery.
- **Loud spec problems:**
  - ERRATA 1 as written makes the failed disk's nodatasum sectors unreadable after a clean remount (1,392 states with no crash). Keep stage 0's rule that classifies only torn records (scrub.c:5637-5640).
  - The pending-window gate leaves finished writes looking torn: 16,884 unreadable states, 11,472 with a "finished" flag.
  - T-fua on the log block turns a 6-write glitch into a permanent failure. That contradicts the design's own "transient: no transition" row.

**Availability on the same fault schedule.** RAID5, 4 stripes, log of 2, 24 writes, d0 failing for good, nodatasum:

| Policy | Writes refused | Crash points losing acknowledged data | Of which gone from disk |
|---|---|---|---|
| A | 18 of 24 | 18 of 24 (all readable with NI) | 0 |
| A + NI | 18 of 24 | 0 | 0 |
| C (T-log trigger) | 2 of 24 | 82 of 88 | 20 |
| C (T-fua trigger) | 1 of 24 | 90 of 160 | 22 |
| C strict | 12 of 24 | 0 of 72 | 0 |

- With checksummed data, C loses 20-24 crash points, all gone from disk; A loses 0.
- Refused share over all sequences:
  - C refuses more than A when a single disk fails: persistent 23.5%† against 16.2%†, transient 19.9%† against 12.4%†.
  - A refuses more only where its log wedges: missing_return_full 56.1%† against 23.7%†, and no spare 26.4%† against 21.0%†.

**Bottom line on C.** C done the md way keeps writing by accepting loud loss on a crash. That is the trade your rule rejects. Build stage 1 for what it does well:
- a durable "failed, replace it" state;
- stopping the log from filling with one disk's names;
- a way past A's wedges.

Build it with:
- phase A refusing until the trigger;
- T-missing instead of ERRATA 2;
- the spec fixes above;
- verified reads from the failed disk.

To keep your rule after the trigger, add SD: refuse a partial write that would have to rebuild the failed disk's committed data. C strict then loses nothing. Over all sequences it refuses about 50% more writes than A+NI (16.1% against 10.9%); in the "failed for good" schedule it refuses fewer (12 against 18 of 24), because A's log stays full of the failing disk's names until a replace.

Only journalling the absent column's data would close the last case, a crash and a disk loss at the same moment. That means a partial-parity log, like md's PPL, and no policy here has one.

## 7. Counterexamples that apply to the real kernel, ranked

Ranking: silent before loud; data A would have kept before data already gone; no alert before an alert; one common fault before several.

| # | Class | What happens | Policies hit | States | Kernel path | Fix |
|---|---|---|---|---|---|---|
| 1 | **CUR-1 / CUR-7**: degraded eviction, then the disk returns | Data written while a disk was missing reads the old platter with no error. A scrub or the next write makes the loss permanent. If the stale column was parity, a later disk loss rebuilds an old version. The alert is gone after the remount in 86-100% of cases, and its advice (reconnect, then scrub) makes the loss permanent. | CUR | 38,464 (34,124†); 30,008 for the parity case | 724 → 801/847 ← 982 ← 3552 ← raid56.c:4939; volumes.c:943 trusts the returning disk | A2 with its rules. Until then: replace the missing devid with a new disk; do not reconnect the old one |
| 2 | **X-1**: log-root superblock tie | A disk starts failing writes after the last transaction commit, then an fsync, then a crash. The mount picks that disk's same-generation superblock, which has no log root, and the fsync'd writes vanish. Checksums do not help. Inferred from code; the model tracks which superblock is picked, not tree contents. | A, CUR, A2 (upstream) | 59,136 of 186,084; 115,568 on RAID6 | tree-log.c:3550-3552; disk-io.c:4198-4206, 4223/4290/4314; volumes.c:1241-1248 | Force a transaction commit when a log commit's superblock missed any device holding the current generation; cover missing devices; skip a generation at degraded mounts; commit the pick at mount |
| 3 | **CUR-6 / CUR-5**: any missing disk, plus a present failing disk | The failing disk's records are evicted and its stale data is read directly, at once. The missing disk can be a RAID1 metadata disk or one briefly unplugged. | CUR | 8,896 (RAID5); 96k (87k†, depth 6) on RAID6 | 724 (one flag for the whole filesystem), pass 1 at 847 | A2's rule: drop only names that are all on devices with `bdev == NULL` |
| 4 | **K3**: capacity halving | The live set outgrows the wide layout. The commit keeps the old block and acknowledges anyway, and after a crash parity is rebuilt from the stale disk. Only rate-limited log lines, no raid56 alert. It is a BUG with CONFIG_BTRFS_ASSERT. | A with its knobs (and A2 on this commit path) | 208 to 35,450 in 8 of 8 families | 527-537, 941, 3831, 5757-5776 | R1 (cap the live set at 82) + R2 (a commit that cannot write the log aborts) |
| 5 | **K4**: user scrub during writes | The scrub rebuilds parity from the stale disk and retires the running transaction's record. Permanent, no alert. | A, CUR, A2 | 11,224; 25,584 with flush faults | scrub.c:587-590, 3843/4004; no generation check at 4793 | R4: compare generations, or commit first |
| 6 | **K5**: full-stripe write and a lost flush | Not logged, so nothing names the lost column, and it reads old data. No raid56 alert. | A, CUR, A2 | 11,080 | raid56.c:4926-4940; 2203-2262 | R5: track and name those writes, or abort the commit |
| 7 | **CUR-4 / CUR-11**: flush loss acknowledged unnamed, or not recorded at all | The column reads the old version at once. | CUR, and A with its knobs | 24,508; 48,260 | 3014/3196/3211; 3016/3026/3049/3198 | D4: `wib_readd_dropped()` fails the commit (read-only) in both plans |
| 8 | **CUR-3**: tree-log replay evicts with every disk present | The failing disk's record is spent during the mount. Needs RAID5/6 metadata or space_cache=v1. | CUR | 1,520 (512†); 3,312 / 2,304 / 13,500 in the deeper runs | tree-log.c:7755; 725; 7544, 7575-7577 | Drop the replay exception; with A2, pull the failing disk |
| 9 | **X-3**: the old disk returns in place of its replacement | A write-failing or missing source keeps a valid superblock; later it is accepted as the member. No alert at all. | A, CUR, A2, OLD (upstream) | 2,534 (failing); 1,392-2,868 (missing) | dev-replace.c:1017-1019, 1058-1059; volumes.c:2249-2268, 902-915, 943-961 | Tombstone every unscratched source; key devices by devid plus uuid |
| 10 | **X-2**: orphan superblock after a degraded session | A crashed commit's superblock lands on one disk, which is missing at the next mount. The degraded session then reuses the generation, and when the disk returns its lineage can be mounted. | A, CUR, A2 (upstream) | 35,092 (A); 32,733 (CUR) | disk-io.c:4223/4290/4314; volumes.c:1241-1248 | Skip a generation at degraded read-write mounts, plus a mount commit |
| 11 | **K1**: race | A failed write's record is evicted before it names the disk. Needs concurrency and a full log. | A, CUR, A2 | 34,738; 43,430 | raid56.c:5076 → 5128; 854 | R3: name the disk under the same lock |
| 12 | **K2**: mount recovery drop | A record that finds no room is dropped, and the mount goes on read-write. Silent at the read, alert at mount. | A with the kernel's recovery | 8,888 | 3763-3782; 7335-7342; 6818-6850 | R6: stay read-only, or use A2's drop and taint |
| 13 | **D6**: replay with a disk missing (code reasoning, not run) | A stripe kept torn and unclassified is spent during the replay, and a degraded read rebuilds from torn parity. | CUR | not run | scrub.c:5640-5659; 7591 | As #8 |
| 14 | **CUR-2, CUR-8, CUR-9, CUR-10**: verdicts, a replace's zeros, stripes kept torn, and torn-only records spent | The value was already lost; an EIO becomes garbage, zeros or an older version. For CUR-10, "fix the disk, then scrub" is safe. | CUR | 4,268 / 2,544 / 640 / T1 | 847 pass 3; 3995; 775/784/856-858 | Never spend them (A) |
| 15 | **A-1** (loud) | A crash after the whole-stripe mark and before the repair: the stripe is undecidable, and acknowledged data reads EIO for good although parity holds it. | A, CUR, A2 | 42,356 (14,102†); 425,724 of 3.7M schedules | raid56.c:4939 before 4977; scrub.c:3523, 5122; raid56.c:3337 | NI (UML arm in RESULTS.md §9) |
| 16 | **A-2** (loud): the degraded write hole | A crash while a disk is missing: data that lived only in the torn parity is gone. | Every log policy | crash_loss 14,200 (9,272†) | scrub.c:5767, 5385, 5122 | SD; PPL for the last case |
| 17 | **A-3 / A-4** (availability) | A full log with a failing disk and no spare: stuck. A full log at replay: the mount fails. | A with its knobs | 2,432 (1,536†); 1,920 (1,536†) | 3636; raid56.c:4743 | A2 |
| 18 | CUR stops at a device scan (availability) | A returning disk that is not opened switches off the eviction, and writes refuse until a remount. | CUR | 5 of 24 writes accepted | volumes.c:959-962; 724 | Test `bdev == NULL` |

## 8. Results that rest on a model divergence

**Found and fixed by the fidelity critic** (`fidelity/policy_model_fid.py`; 204 re-runs):

| Divergence | Model vs kernel | Results it changed | After correction |
|---|---|---|---|
| **F1** | The kernel's phase A also writes back the target column when the record names it (raid56.c:4155-4173, 4675-4812); the model skipped it (policy_model.py:523, 643). | The model acknowledged writes into a named column on a still-failing disk, which the kernel refuses. After a heal, a crash in phase B left a verdict where the kernel recovers. | A-1 falls about 3x: persistent 42,356 → 14,102; transient 55k → 19k. The class stays real. "A+NI refuses 45% fewer" becomes 38% fewer (16.2% against 10.9% of writes). |
| **F2** | A runtime detach is not a missing device for the RAID5/6 I/O code: bdev stays set, so the phase-A write-back fails and the write is refused. The model treated it as missing at mount. | Degraded-write-hole losses in the detach families | LOST(gone) falls 35-45%; the hole needs a degraded **mount**. `missing_devices` still rises on unplug, so CUR-1 and CUR-6 through a hot-unplug still stand. |
| **F3** | RAID6 recovery with a spare parity is PROVEN, not TORN (scrub.c:3504-3523); a present named column gets a verdict. | CUR's 32 STUCK states in raid6_bad_missing_full; CUR-2's eviction order | The 32 STUCK vanish. **The attack_CUR run's 194 STUCK at depth 7 has the same cause and was not re-run; treat it as an artifact.** attack_A's STUCK of 1,158-4,294 in cap_raid6 was flagged by attack_A itself as this artifact. |

**What survived correction:**
- all five original CUR silent classes and their shortest traces, unchanged;
- 0 silent states for A and A2 in every cell;
- C's degraded write hole (16,144 gone against A's 0);
- ERRATA 2's silent drop (19k states).

**Not modelled in the shared matrix, argued from code or modelled by one attacker only:**
- **D4, the LOSE plan.** The shared model's "A" silently assumed a kernel change. With only the two knobs, A is still silent (control K_a_ack_unnamed: 24,508). attack_CUR modelled LOSE as CUR-11.
- **D5, capacity halving.** K3 rests on attack_A's model of it; the critic checked the kernel lines, not that model.
- **D6:** an extra CUR path, not run (#13 above).
- **D7-D10:** minor.
  - D7: the kernel does not refuse one rebuild that the model refuses; the critic judged the result loud.
  - D8: the kernel keeps the torn mark after a successful RMW.
  - D9: the kernel records no verdict when there are more unknowns than parities.
  - D10: the kernel writes back a checksummed column that fails its checksum.
- **D11: the kernel forces copy-on-write on RAID5/6** (inode.c:1880-1920). The model's in-place cells stand for other rows of the same column; the classes hold at column and stripe level.
- **D12: CUR-3 and A-4 need metadata on RAID5/6.** With RAID1 metadata neither arises.

**Other artifacts that are not claimed as findings:**
- C and CB silent states with two flush-failing disks (12,834 and 10,361): a naming shortcut in the shared model (attack_B; 0 with `--fix-cflush`).
- C and A silent in a RAID6 triple fault (506 and 52): depends on the model dropping a DECIDED record (attack_C).
- CTM's low counts in missing_return_twice and raid6_second: a device-budget artifact.
- Raw REF_WRITE counts across policies: not comparable; use the refused shares.

## 9. What I would do

1. **Make refusal the production default**, not a debug knob, and add the fixes A needs anyway:
   - D4: abort the commit instead of acknowledging an unnamed or lost flush;
   - R1: cap the live set at the wide layout's 82 entries;
   - R2: a commit, a replace's marks or mount recovery that cannot keep a record refuses;
   - R3: name the disk under the same lock (race K1);
   - R4: scrub compares generations or commits first (K4);
   - R5: track unlogged full-stripe writes (K5);
   - R6: mount recovery stays read-only rather than drop (K2).
2. **Replace CUR's degraded and replay exceptions with A2**, with every rule in section 5. Use a per-region taint with a resync on return when stage 2's map exists.
3. **Implement NI.** It turns A's main loud loss (A-1) into readable data and cuts refused writes. First reproduce A-1 with the UML arm in RESULTS.md §9.
4. **Stage 1 (C):**
   - phase A refuses until the trigger;
   - use T-missing, not ERRATA 2;
   - apply the four spec fixes, the ERRATA 1 torn-only rule and verified rebuild sources;
   - add SD if the array must keep your rule after the trigger.
5. **Fix the three upstream holes:** X-1 (force a commit), X-2 (generation skip plus mount commit), X-3 (tombstone).
6. **Alerts:** persist the latch across mounts. For a dropped record naming a missing disk, the advice should be "replace that devid with a new disk; do not reconnect the old one".
7. **For an admin on today's kernel:**
   - use checksummed data where you can: CUR's silent classes then become EIO;
   - keep metadata on RAID1 or RAID1C3, which removes CUR-3 and A-4;
   - after running degraded, replace the missing devid with a new disk instead of reconnecting the old one;
   - do not scrub a stripe set while a disk is failing writes into it.
8. **Before relying on any of this, reproduce on UML:** A-1, K3, CUR-6 and X-1. The arms are written in the agents' outputs.

## Table

Each cell gives the finding and the state count behind it. † = fidelity-corrected value; the first number is the original run. RAID5 and nodatasum unless stated. Counts are distinct states (refusals are distinct transitions), so compare zero against non-zero, not the sizes.

| Policy | Silent wrong reads | Acknowledged data gone from every disk | Acknowledged data on disk but unreadable | Refused writes / read-only | Stuck | Needless EIO |
|---|---|---|---|---|---|---|
| **OLD** (upstream, no log) | **Everywhere.** 310k states (persistent), 258k (flush), 35k (the classic write hole). Checksummed: 0 | 206k (persistent); checksummed 18k | 65k | Never refuses | 0 | 0 |
| **CUR** (kernel today) | **11 classes.** 38,464 (34,124†) degraded eviction then the disk returns; 24,508 unnamed flush; 48,260 flush dropped outright; 1,520 (512†) replay; 8,896 unrelated disk missing; 96k (87k†, depth 6) RAID6. 0 with every disk present and no replay. Checksummed: 0 | missing_return_full 32k (28k†), where A has 1,312 (768†) | persistent 42,356 (14,102†), the same as A | The same as A with every disk present: persistent 81k (63k†), 16.2%† of writes. While degraded it keeps writing (24 of 24). The mount fails at replay in 384 cases | 0 (the 32 RAID6 states were a model artifact†) | 42,356 (persistent); 16,896 (missing_return_full) |
| **A** (strict refusal, as modelled) | **None** from the log's rules | Only the degraded write hole: crash_loss 14,200 (9,272†); missing_return_full 1,312 (768†); 0 with every disk present | A-1: persistent 42,356 (14,102†); 0 with NI | persistent 81k (63k†), 16.2%†. Degraded with a full log: 56.1%† (3 of 24 accepted). Metadata read-only 11k; the mount fails at replay 1,920 (1,536†) | 2,432 (1,536†): a failing disk and no spare | 42,356 (persistent) |
| **A + NI** (not implemented) | None | As A | **0** in persistent | 44,724 (39k†); 10.9%† | Not run | 0 (persistent) |
| **A2 as specified** | 0 in the shared matrix; the attack found 4 holes: 7,650 (a replace's own zeros), 3,456 (taint not yet on disk; 91,111 / 589,033 bad mounts in the persistence model), 12,030 (resumed scrub), plus 1,476 over A when an old disk returns after a replace | missing_return_full 7,120 (6,576†); 3,240 after two absences in turn (A: 0) | 8,824 after two absences (A: 0); 6,776 when a second disk dies after the return (A: 0); 2,112 lost only because of the whole-disk taint | Degraded: 24 of 24 accepted (A: 3 of 24). After the return: 10 of 24 (A: 17 of 24) unless repair may write onto a tainted disk. At replay the mount fails until the failing disk is pulled | 0 (A: 2,432) | 22,608 (missing_return_full) |
| **A2 with all fixes** | **0** in all 22 families run | 0 after two absences, and 0 after a second disk dies; missing_return_full 8,128 (A: 1,312), because it keeps writing while degraded | 0 after two absences; missing_return_full 17,120 (A: 2,912) | 0 refused after two absences (A: 8,288) and after the return (A: 10,512). Still refuses as A does when verdicts fill the log after a crash while degraded, or on RAID6 with a present failing disk | 0 where A is not stuck | Not reported |
| **B** (per-stripe degrade) | 0, but only because of the torn-read refusal and the §3.5 verdict. A literal "never refuse" B: 150,228 of 544,942 schedules silent | persistent 19k. **One failing disk plus one crash: 212,862 of 3,717,550 schedules (A: 0).** Metadata: 28,858 schedules (A: 0 lost, read-only instead) | persistent 45k | persistent 63k | 2,432 with no spare | 48,572 |
| **C as designed** (§2.1 phase A never refuses before the trigger; ERRATA 2 and 6 as written) | **Yes.** ERRATA 2 untainted drop 24,320 (19k†); ERRATA 6 gap, fsync'd data lost with the log root, 26,944-156,992; superblock-pick holes 680 and 1,541 | CB persistent 49k; C 19k (16,144†); the same 212,862 schedules as B; metadata 2,032-2,184 lost where A goes read-only | persistent 129k (63k†) | 23.5%† of writes (A: 16.2%†); in the "failed for good" schedule 1-2 of 24 refused, but 82-90 crash points lose data | 1,788 (checksummed): read-only for good after a second disk loss | 134,450 (persistent) |
| **C, spec fixed + T-missing** (phase A refuses until the trigger) | **0** in every run | Still loses: persistent 16k†, metadata 2,184, RAID6 with two disks failed 1,912 (A: 0) | persistent 63k†; RAID6 with two disks failed 59,844 (A: 14,568) | Few refused | Read-only for good after a second loss | 1,832 against 1,000 when the failed disk is never used as a rebuild source |
| **C strict** (C + SD + NI + the checksum case) | 0 | **0** | **0** (persistent) | 54,580 against A+NI's 33,716 transitions; 16.1%† against 10.9%† of writes; 12 of 24 in the "failed for good" schedule (A: 18 of 24) | Not reported | Not reported |

**Per-family counts from the shared matrix** (original runs, nodatasum). Each cell reads SILENT / GONE / UNREADABLE / STUCK / REF_WRITE / RO; k = thousands; T6 = stopped on memory at depth 6.

| Family | OLD | A | CUR | A2 | B | C | CB | CTM |
|---|---|---|---|---|---|---|---|---|
| persistent (d10) | 310k/206k/65k/0/0/0 | 0/0/42k/0/81k/0 | = A | = A | 0/19k/45k/0/63k/0 | 0/19k/129k/0/180k/0 | 0/49k/134k/0/174k/0 | = C |
| flush_unnamed (d9) | 36k/23k/7384/0/0/0 | 0/0/0/0/4472/4472 | 25k/16k/8494/0/0/0 | = A | = A | 0/1472/6008/0/6880/0 | = C | = C |
| crash_loss (d10) | 35k/35k/0/0/0/0 | 0/14k/27k/0/47k/0 | = A | = A | = A | = A | = A | = A |
| missing_return_full (d10, cap 1) | 35k/25k/9680/0/0/0 | 0/1312/2912/0/28k/0 | 38k/32k/21k/0/52k/0 | 0/7120/22k/0/98k/0 | = A | 24k/21k/19k/0/72k/0 | = C | 0/3312/9936/0/40k/0 |
| cap2_missing_return (d7) | 6032/2908/3124/0/0/0 | 0/0/0/0/8028/0 | 1788/576/1212/0/0/0 | 0/0/0/0/288/0 | = A | = CUR | = CUR | 0/0/0/0/0/0 |
| replay_full (d9, cap 1) | 16k/11k/256/0/0/0 | 0/0/1536/0/14k/1920 | 1520/756/512/0/18k/384 | 0/0/512/0/14k/1920 | 0/256/1536/0/12k/1536 | 0/760/4076/0/16k/384 | 0/1272/4080/0/16k/0 | = C |
| nospare_persistent_full (d10) | 16k/11k/256/0/0/0 | 0/0/768/2432/13k/0 | 0/0/768/0/13k/0 | = CUR | 0/256/768/2432/12k/0 | 0/768/4736/0/18k/0 | 0/1280/4736/0/18k/0 | = C |
| raid6_bad_missing_full (d7) | 443k/199k/113k/0/0/0 T6 | 0/5096/39k/0/274k/0 | 96k/33k/130k/32/146k/0 T6 | 0/8488/84k/0/377k/0 | 0/13k/47k/0/294k/0 | 0/22k/164k/0/236k/0 T6 | 0/31k/168k/0/228k/0 T6 | 0/34k/201k/0/372k/0 |
| meta_persistent_full (checksummed metadata, d9) | 0/2032/0/0/984/984 | 0/0/0/0/11k/11k | = A | = A | 0/896/0/0/10k/10k | 0/2032/0/0/5108/5108 | 0/2544/0/0/3720/3720 | = C |

## Counterexamples

### CUR: SILENT_WRONG, then LOST(gone) after a scrub or the next write (Rank 1: silent loss of data A keeps. 38,464 states (34,124 corrected) in missing_return_full; 30,008 in the parity case. The in-memory alert is gone at the remount (86-100% of these wrong reads show no alert). The printed advice (reconnect or fix, then scrub) makes the loss permanent in 100% of sampled drops; only replacing the missing devid with a new disk is safe. Fix: A2 with its rules (drop only names all on devices with bdev == NULL, a durable taint, per-region resync on return).)

Scenario: Rank 1. CUR-1 / CUR-7: degraded eviction, then the missing disk returns

Trace: d0 detaches || write s0.d0=2 ok || write s1.d0=2 (evicts s0[d0]) ok || unmount ; mount with {d0} back  => s0.d0 reads 1 (acknowledged 2), no error; '|| scrub' folds the stale d0 into P and the value is gone. Parity case (CUR-7): P detaches || write s0.d0=2 || write s1.d0=2 (evicts s0[P]) || remount with P back || d0 detaches => s0.d0 is rebuilt from the stale P

Kernel: Yes. wib_may_evict_naming() raid56-wib.c:716-726 (missing_devices at 724) -> wib_evict_sticky() 801, pass loop 847 <- wib_find_or_alloc_entry() 982 <- btrfs_wib_mark() 3552 <- rmw_rbio() raid56.c:4939. device_list_add() trusts the returning disk (volumes.c:943). Alert latch in memory only (raid56-wib.h:813). Confirmed by the fidelity critic.

### A, CUR, A2 (upstream btrfs): SILENT_WRONG (fsync'd data lost with the log root) (Rank 2: silent, and checksums do not help. 59,136 of 186,084 states (RAID5), 115,422 transient, 115,568 RAID6. The only sign is a device write-error message. Inferred: the model tracks which superblock is picked, not tree contents. Fix: force a transaction commit when a log commit's superblock missed any device holding the current generation; also cover missing devices, add a generation skip at degraded read-write mounts, and a mount commit (all 0 in the model).)

Scenario: Rank 2. X-1: log-root superblock tie

Trace: d0 starts failing writes (after the last transaction commit) || write s0.d1=2 ; LOG commit ok (the superblock write to d0 fails and is tolerated) || CRASH ; mount (tie: the superblock of d0 is picked) => generation G, no log root: s0.d1 reads 1 (acknowledged 2)

Kernel: Yes (upstream code kept by stage-0). tree-log.c:3550-3552; the fsync path writes the primary copy only (disk-io.c:4198-4206) and tolerates num_devices-1 errors (4223/4290/4314); latest_dev is chosen by a strict '>' on generation in list order (volumes.c:1241-1248, 689; disk-io.c:3413). Not reproduced on UML.

### CUR: SILENT_WRONG (Rank 3: silent at once, and the loss is of data A keeps. 8,896 states (RAID5, d9); 96k (87k corrected, stopped at depth 6) on RAID6. No admin response after the alert helps. Fix: A2's rule (drop only records whose every stale bit is on a device with bdev == NULL).)

Scenario: Rank 3. CUR-6 / CUR-5: any missing device plus a present, failing disk

Trace: d0 starts failing writes || write s0.d0=2 ok (write to d0 failed) || X detaches (X holds no column of this chunk) || write s1.d0=2 (evicts s0[d0]) ok  => s0.d0 reads 1 directly from the present d0

Kernel: Yes. missing_devices is one count for the whole filesystem: raised at mount (volumes.c:7551) and by runtime unplug (btrfs_remove_bdev, super.c:2524). Pass 1 of wib_evict_sticky() (raid56-wib.c:847) spends the naming record.

### A as the kernel implements it (keep_naming_degraded=1, torn_unevictable=1); also A2 on the same commit path: SILENT_WRONG with no raid56 alert, then LOST(gone) (Rank 4: silent in 8 of 8 capacity families (208 to 35,450 states). It is a BUG with CONFIG_BTRFS_ASSERT. Fix: R1 (never admit more records than the wide layout's 82 entries) plus R2 (a commit, a replace's marks or mount recovery that cannot write the log refuses). A_strict and R1 give 0.)

Scenario: Rank 4. K3: the log capacity halves and the commit ignores a full block

Trace: write s0.d0=2 ; CRASH after mark (the log also lists s1) ; mount without d0 || write s2.d0=2 [names d0: the set needs the wide layout and no longer fits; 'block full, keeping the previous one'] ok || unmount ; mount with {d0} back => recovery rebuilds s2's P from the stale d0; s2.d0 reads 1

Kernel: Yes. wib_live_max() raid56-wib.c:527-537 (165 -> 82); wib_enforce_capacity_locked() 941 (called at 3831); btrfs_wib_commit() warns and returns 0 on -ENOSPC (5757-5776, checked). Kernel trigger: a crash with more than 82 regions listed, a degraded mount, then a write touching the missing device. The replace variant (K3r) goes through btrfs_wib_replace_mark_stale() 3910/3942.

### A, CUR, A2 (policy-independent): SILENT_WRONG with no alert; LOST(gone) (Rank 5: silent and permanent. 11,224 states (25,584 with flush faults); checksummed data: a loud loss. Fix R4: scrub keeps records newer than the commit root (compare generations), or commits after marking the block group read-only, as replace does.)

Scenario: Rank 5. K4: a user scrub between a write's RMW and the commit that acknowledges it

Trace: d0 starts failing writes || write s0.d0=2 ; SCRUB before the commit (the commit root shows s0.d0 as free) regenerates P from the stale d0 and retires the record ; ok => s0.d0 reads 1, and 2 is on no disk

Kernel: Yes (a plausible window). scrub.c:587-590 and 3773-3775 search the commit root; scrub_raid56_plan_wib() 3357; btrfs_wib_clear_sticky() at scrub.c:4003-4004/3843 with no generation check (raid56-wib.c:4793).

### A, CUR, A2: SILENT_WRONG with no raid56 alert (Rank 6: silent. 11,080 states; 0 when the loss is named. Fix R5: track full-stripe writes as written and name them after a failed flush, or abort the commit.)

Scenario: Rank 6. K5: an unlogged full-stripe COW write whose column a failed flush loses

Trace: d0 starts failing flushes || fullwrite s0=[2,2] ok (the flush lost d0's write; the stripe is not listed, so nothing names it) => s0.d0 reads 1

Kernel: Yes. Full-stripe writes are not logged (raid56.c:4926-4940); btrfs_wib_note_written() keeps nothing for an unlisted region (raid56-wib.c:2203-2262).

### CUR, and A with its knobs: SILENT_WRONG (Rank 7: silent, and even the kernel configured as A keeps it (control K_a_ack_unnamed: 24,508). LOSE plan: 48,260. Fix D4: wib_readd_dropped() fails the commit (read-only) in both the UNNAMED and LOSE plans.)

Scenario: Rank 7. CUR-4 / CUR-11: a flush loss acknowledged without a name, or not recorded at all (LOSE plan)

Trace: d0 starts failing flushes || write s0.d0=2 (the flush lost d0's write; the log cannot name it, or has no room) ok => s0.d0 reads 1

Kernel: Yes. UNNAMED plan raid56-wib.c:3014, EV_LOG_UNFLUSHED 3196, message 3211; LOSE plan 3016/3026, nr_lost 3049, EV_DROPPED 3198. No knob gates either. How often they occur depends on concurrency the model switches on rather than derives.

### CUR: SILENT_WRONG (Rank 8: silent inside mount(). 1,520 states (512 corrected); 3,312 / 2,304 / 13,500 in the deeper runs. Needs RAID5/6 metadata or space_cache=v1 (D12). Fix: drop the replay exception; the mount fails until the failing disk is pulled, then A2's drop and taint lets it proceed.)

Scenario: Rank 8. CUR-3: tree-log replay evicts records with every disk present

Trace: d0 starts failing writes || write s0.d0=2 ok (write to d0 failed) || fsync s1.d0=2 (tree log) ; CRASH ; mount ; replay s1.d0=2 (evicts s0[d0]) => s0.d0 reads 1

Kernel: Yes. BTRFS_FS_LOG_RECOVERING set and cleared at tree-log.c:7755 and 7908, tested at raid56-wib.c:725; error records wait in RECOVER_VERIFY (7544) and are put back live (7575-7577). Confirmed by the fidelity critic.

### A, CUR, A2, OLD (upstream): SILENT_WRONG with no alert at all (Rank 9: silent, no alert. 2,534 states (failing case); 1,392-2,868 (missing case). Fix: keep a tombstone for every replace whose source superblock was not scratched, and key devices by devid plus uuid.)

Scenario: Rank 9. X-3: a replaced (failing or missing) disk comes back in place of its replacement

Trace: d0 starts failing writes || write s0.d0=2 ok (write to d0 failed) || replace d0 || unmount ; mount without the replacement, old d0 accepted => s0.d0 reads 1

Kernel: Yes. dev-replace.c:1017-1019 (uuid swap), 1058-1059 (scratch only if WRITEABLE); btrfs_scratch_superblock only warns on failure (volumes.c:2249-2268); device_list_add 902-915 and 943-961.

### A, CUR, A2 (upstream): SILENT_WRONG (the orphan superblock's lineage is mounted) (Rank 10: silent. A: 35,092 states; CUR: 32,733 (depth 5). Fix: a generation skip at degraded read-write mounts plus a mount commit (0).)

Scenario: Rank 10. X-2: orphan superblock after a degraded session

Trace: txn commit ; sb landed only on d0 ; CRASH ; mount without d0 || write s0.d0=2 ; LOG commit ok || unmount ; mount with {d0} back (tie: d0's superblock is picked) => the degraded session's commits and acknowledged writes are gone

Kernel: Yes, inferred from code: write_all_supers tolerates num_devices-1 errors (disk-io.c:4223/4290/4314); latest_dev by generation in list order (volumes.c:1241-1248).

### A, CUR, A2: SILENT_WRONG (record_dropped raised), later LOST(gone) (Rank 11: silent; needs concurrent failing RMWs and a full log. 34,738 / 43,430 states. Fix R3: name the member under the same lock that clears the in-flight bit.)

Scenario: Rank 11. K1: race between btrfs_wib_done(failed) and rmw_update_stale_data() with a full log

Trace: d0 starts failing writes || write s0.d0=2 RACE (a concurrent mark evicts the still-vague record) ok => s0.d0 reads 1

Kernel: Yes (a race). raid56.c:5076 -> 5128/5129; raid56-wib.c:3691-3725; pass 0 at 854; the full-stripe path at raid56.c:5154-5168.

### A with the kernel's mount recovery (also under R1): SILENT_WRONG at the read (alert at mount) (Rank 12: 8,888 silent states. Fix R6: recovery stays read-only rather than drop, or uses A2's drop and taint. A_strict: 0 silent, 824 STUCK.)

Scenario: Rank 12. K2: mount recovery drops a record it cannot fit (the pending set is a union of devices' newest blocks)

Trace: write s0.d0=2 ; phaseB landed {d0} ; CRASH (an older block on one device lists s1) ; mount without d1 ; no room for s0's verdict: dropped => s0.d1 is rebuilt from the torn P

Kernel: Yes. btrfs_wib_add_sticky() raid56-wib.c:3763-3782; wib_recover_one() turns -EIO into 'kept' (7335-7342); union at 6818-6850.

### CUR: SILENT_WRONG (code reasoning, not run) (Rank 13: silent, CUR only. Fix: the same as CUR-3 (no replay exception); A and A2 never drop torn records.)

Scenario: Rank 13. D6: tree-log replay with a device missing

Trace: crash with a write in flight into a stripe whose data column is on a device missing at the next mount || the mount with a replay pending keeps the stripe as plain @torn (unclassified) || the replay's RMW finds the log full and pass 1 spends it || a degraded read rebuilds from the torn parity

Kernel: scrub.c:5640-5659 (classify = !log_replay_pending); raid56-wib.c:7591, 725, 847-862. Found by the fidelity critic; not in any matrix family.

### CUR: SILENT_WRONG (garbage, zeros or an older version in place of an EIO) (Rank 14: the value was already lost (loud under A); CUR makes it silent. 4,268 (D2), 640 (T2); zeros served as data after a replace. For CUR-10, 'fix the device, then scrub' before the next disk loss is safe. Fix: never spend verdicts, replace marks, kept_torn or torn-only records (A).)

Scenario: Rank 14. CUR-2 / CUR-8 / CUR-9 / CUR-10: verdicts, a finished replace's zero marks, stripes kept torn, and torn-only records are spent

Trace: CUR-2: write s0.d0=2 ; phaseB landed {d0} ; CRASH ; mount without d1 || write s1.d0=2 (evicts s0's verdict) => s0.d1 reads a rebuild from the torn P. CUR-9: sector s0.d0 unreadable || write s0.d1=2 ; CRASH ; mount (kept torn) || write s1.d0=2 (evicts) => garbage

Kernel: Yes. Pass 3 at raid56-wib.c:847 (warning 871); btrfs_wib_replace_end() turns marks into ordinary records (3995, 4026); wib_entry_torn_only() 775, kept_torn 784, passes 856-858, fallback 3589-3594. CUR-2's eviction order was corrected by F3 (verdicts are spent last); the class stands.

### A, CUR, A2 (all log policies): LOST_ACKED (unreadable; the data is still in parity) (Rank 15: loud, but the largest cost of strict refusal. 42,356 states, corrected to 14,102 (F1); 425,724 of 3.7M schedules. Fix: NI (mark only the columns phase B writes, after phase A's persist). A+NI: 0. Not implemented; a UML arm is in RESULTS.md §9.)

Scenario: Rank 15. A-1: a crash after the whole-stripe in-flight mark and before phase A's repair

Trace: d0 starts failing writes || write s0.d0=2 ok (write to d0 failed) || write s0.d0=3 (or repair s0) ; CRASH after mark ; mount => SCRUB_WIB_TORN, s0.d0 reads EIO forever although P holds 2

Kernel: Yes. btrfs_wib_mark() at raid56.c:4939 before rmw_repair_first() at 4977; scrub.c:3523 and 5122; read refusal raid56.c:3337.

### Every log policy: LOST_ACKED (gone; loud EIO) (Rank 16: loud. crash_loss 14,200 (9,272 corrected; F2 shows it needs a degraded mount, not just a hot-unplug). Fix: SD (refuse partial writes that must rebuild an absent column with committed data); a crash and a disk loss at the same moment needs a partial-parity log (PPL).)

Scenario: Rank 16. A-2: the degraded write hole

Trace: d0 missing (degraded mount) || write s0.d0=2 ok || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount => d0's value was only in P, and P is torn

Kernel: Yes. Degraded RMWs are allowed; recovery classifies the stripe (scrub.c:5767, 5385, 5122). This is the documented [LIMITS] of raid56-wib.c.

### A (the kernel with raid56_keep_naming_degraded=1): STUCK / REFUSED_WRITE / mount failure (Rank 17: availability only. STUCK 2,432 (1,536 corrected), 5,116 with a 5-action admin closure; the mount fails in 1,920 (1,536). Fix: A2 (pull the disk, then drop and taint).)

Scenario: Rank 17. A-3 / A-4: a failing disk, a full log and no spare; a full log at tree-log replay

Trace: P starts failing writes || write s0.d0=2 ok [the log is full of P's name] || every write into a new region REFUSED(log-full), and with no spare no admin sequence restores writes. Replay: ... fsync ; CRASH ; mount ; replay REFUSED(log-full): the mount fails

Kernel: Yes with the debug knob: btrfs_wib_mark() fails at WIB_ROOM_NONE (raid56-wib.c:3636); rmw_repair_first() refuses at raid56.c:4743.

### CUR (and A2 if keyed on the MISSING bit): REFUSED_WRITE (Rank 18: availability (safe, surprising): 5 of 24 writes accepted, against 24 of 24 for A2 keyed on bdev == NULL. Fix: define missing as bdev == NULL.)

Scenario: Rank 18. A missing disk is plugged back in while mounted; a device scan re-registers it unopened

Trace: d0 missing at mount; 2 writes; d0 re-appears to a device scan => every new-region write REFUSED(log-full) until a remount

Kernel: Yes. device_list_add() clears MISSING and decrements missing_devices while bdev stays NULL (volumes.c:959-962); wib_may_evict_naming() rereads it (raid56-wib.c:724).

### A2 as specified (design only): SILENT_WRONG (7,650 states. Fix: keep CUR's replace_marks_lost rule (the replace fails), and let a replace clear only an older taint. With these: 0.)

Scenario: A replace's own zero marks look missing-only; the finished replace clears the taint

Trace: d0 detaches || write s0.d0=2 || write s1.d1=2 evict s0[0] taint{d0} || remount with d0 back || d1 detaches || replace d1 => the replace writes zeros for s0.d1 and marks {d1}; the mark is evicted with taint{d1}; the finished replace clears the taint; s0.d1 reads zeros

Kernel: A2 is not implemented. wib_entry_replace_owned() and replace_marks_lost (raid56-wib.c:801-901, 4037-4043) already exist and must be kept.

### A2 as specified (design only): SILENT_WRONG (3,456 states (91,111 / 589,033 bad mounts in the persistence model; 6,246 / 42,651 with the superblock one-copy rule); 12,030 for the scrub case. Fix: the taint in every log block header (or a superblock with zero write errors); scrub epochs.)

Scenario: The taint reaches disk after the drop; or a resumed scrub clears a newer taint

Trace: d0 detaches || write s0.d0=2 || write s1.d0=2 evict s0[0] taint{d0} (not durable) ; CRASH after mark ; mount with d0 back => s0.d0 reads 1. Scrub: scrub start || d0 detaches || ... taint{d0} || remount with d0 || scrub resume ends the pass and clears the taint => s0.d0 reads 1

Kernel: Design rule. btrfs_wib_mark() writes the block synchronously with FUA (raid56-wib.c:3262); the header has flags and reserved space (raid56-wib.h:186-199).

### B, and C as designed (§2.1): LOST_ACKED (gone; loud) (212,862 of 3,717,550 schedules (A: 0); metadata 28,858 of 544,942 (A: read-only, 0 lost). Fix: ship stage 1 with raid56_wf_phase_a_refuses=1 as the default.)

Scenario: B-1: RAID5, one failing disk, one crash, every disk present

Trace: d0 starts failing writes || write s0.d0=2 ok (2 only in P) || write s0.d1=2 phaseA {d0} refused->degrade ; phaseB landed {d1} ; CRASH ; mount => 2 is on no disk

Kernel: Not in today's kernel: rmw_repair_first() refuses at raid56.c:4743. The debug knob raid56_rmw_single_phase (raid56.c:4624) reproduces the class.

### C (design stage 1): SILENT_WRONG (26,944-156,992 states (log root); 24,320 (19k corrected), and 11,344 even under SD+NI (ERRATA 2). Fixes: the ERRATA 6 'full' rule extended to missing devices; T-missing or A2's taint; a generation skip; a mount commit.)

Scenario: Spec holes: ERRATA 6 misses devices marked failed outside write_all_supers; ERRATA 2 drops without a taint

Trace: d0 starts failing writes || write s0.d0=2 T-fua(log) fails d0 ; LOG commit ok || CRASH ; mount (tie: d0's superblock) => log root missing. ERRATA 2: d0 detaches || write s0.d0=2 || write s1.d0=2 evict s0[0] ; remount with d0 back => s0.d0 reads 1

Kernel: Design only; X-1 is the same mechanism in today's kernel.

### C (design stage 1, even with every fix): LOST_ACKED (gone) / read-only for good (persistent 16,144 gone (corrected; A: 0); metadata 2,184 gone where A goes read-only; RAID6 with two disks failed 1,912 gone; 1,788 stuck (checksummed). Fix: SD (C strict: 0 loss) and verified rebuild sources.)

Scenario: C-1, C-4, C-5: the degraded write hole on the failed disk; a second loss; metadata

Trace: d0 failing || trigger fail d0 || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount => d0's acknowledged value is gone. A second loss: fail d0 || d1 detaches => read-only for good

Kernel: Design only.

## Sensitivity

**Where the numbers come from.** The synthesis runs no new models. It uses the seven agents' structured results. I extracted them in full from the workflow journal into `policy-compare/synth_work/*.txt`; the attack folders have no REPORT.md files, so these are the only complete on-disk copies.

**Negative controls: each failure class is visible to the model.**

| Source | Control | Result |
|---|---|---|
| Shared model | 22 negative controls | 21 fire. The one that does not (A + read_trust_torn = 0) is explained: under A, phase B never runs over a named present column. |
| Shared model | Checksummed runs | All 0 silent |
| Shared model | Re-runs with values that cannot alias | Same verdicts |
| Fidelity critic | Every A, CUR and A2 control on the corrected model | All still fire, e.g. K_cur_degraded_evict 34,124; K_a_ack_unnamed 24,508; K_a2_no_taint 19,380 |
| Fidelity critic | `--fid none` | Reproduces the original exactly |
| Fidelity critic | Each fix alone | Changes only its target: F1 the A-1 counts, F2 detach losses, F3 CUR's 32 STUCK |
| attack_A | One on/off pair per new class | Every class disappears when its mechanism is off: K3 A_strict 0, R1 0; K1, K4 and K5 0 without the window |
| attack_CUR | Ablations | "only X" reproduces CUR's count and "without X" gives 0. This attributes every silent state to exactly one exception |
| attack_CUR | `--alert-volatile` | Shows the alert missing in 19-100% of silent reads |
| attack_A2 | Each A2 rule | Silent without it (3,456-25,392 states), 0 with it |
| attack_A2 | Taint-placement control in the persistence model | 30,598 / 329,987 violations |
| attack_B | Its silence controls | read_trust_torn and no_suspect make exactly B's losing schedules silent (3,428 of 3,428) |
| attack_B | History bits | Attribute every B loss to the degrade |
| attack_C | Each spec fix, controls removing it | Silent from 680 up to 156,992 states without it, 0 with it |
| attack_C | c_trust_unverified | Still 171,592 silent |

**Regression checks.** Each attack model with its new switches off reproduces policy_model.py exactly: attack_A 6/6, attack_CUR 4/4, attack_A2 8/8, attack_B 6/6, attack_C 8/8.

**Kernel spot-check by me.** The branch is still at 2a1e2379e4, the same as the one cited. I checked three cited sites:
- raid56-wib.c:716-726 tests missing_devices and LOG_RECOVERING;
- btrfs_wib_commit() returns 0 after "block full, keeping the previous one";
- the refusal knobs sit under CONFIG_BTRFS_DEBUG and are const false otherwise.

## Caveats

**COMPARISON.md was not written to disk.** The system rules for this subagent forbid writing report or summary .md files, and the harness refused the same for every sibling agent. The complete document is in `summary`, ready for the parent to save verbatim.

**What the models cover.**
- Tiny geometry: one sector per column, 2 data columns, 2-3 stripes, no parity rotation.
- One operation at a time. Replace and scrub are atomic (except where noted).
- LOST and STUCK are judged with an admin closure of at most 3 actions (5 in one check).
- The log is counted in records. The kernel's 64-block, 4 MiB entries and its 165/82 two-layout capacity are modelled only in attack_A; the kernel numbers are inferred from raid56-wib.h.
- Races (K1, K4) are single injected interleavings, so their frequency is unknown.
- CUR-4 and CUR-11 depend on a condition the models switch on rather than derive.
- X-1 and X-2 are judged on the superblock pick, not simulated tree contents.

**Counts.**
- Counts are distinct states over different state spaces. Compare zero against non-zero and the traces, not magnitudes; use refused shares for availability.
- Several runs stopped on memory limits (marked T); their counts are exact up to that depth.

**Fidelity-flagged results** (section 8 of the summary):
- A-1's magnitude is about 3x too high in the original matrix (F1).
- Degraded-write-hole losses in detach families are 35-45% too high (F2).
- CUR's RAID6 STUCK states (32 in the matrix, 194 in attack_CUR) and attack_A's cap_raid6 STUCK states are model artifacts (F3).
- The shared model's A assumed the D4 kernel fix.
- CUR-3 and A-4 need RAID5/6 metadata (D12).
- C's silent states with two flush-failing disks are a shared-model artifact.
- The C/A RAID6 triple-fault states are model-dependent and not claimed.

**Not validated on a kernel.**
- Nothing was reproduced on a UML kernel.
- A2, NI, SD, C's spec fixes, and fixes R1-R6 and D4 exist only in the models.
- The "recommended A2" per-region taint with resync stands for the stage-2 map; its real cost (resync time, bitmap persistence) is not modelled.
- Kernel line numbers are at stage0-wip 2a1e2379e4. I read only; nothing under /home/user was modified.

## Model files

- <scratch>/policy_model.py
- <scratch>/RESULTS.md
- <scratch>/summary.md
- <scratch>/fidelity/policy_model_fid.py
- <scratch>/fidelity/out/
- <scratch>/attack_A/attack_model.py
- <scratch>/attack_CUR/cur_model.py
- <scratch>/attack_CUR/summary.txt
- <scratch>/attack_A2/a2_model.py
- <scratch>/attack_A2/tp_model.py
- <scratch>/attack_A2/wedge.py
- <scratch>/attack_A2/final_table.md
- <scratch>/attack_B/b_model.py
- <scratch>/attack_B/sched.py
- <scratch>/attack_C/c_model.py
- <scratch>/attack_C/summary_table.txt
- <scratch>/synth_work/model.txt
- <scratch>/synth_work/break_A.txt
- <scratch>/synth_work/break_CUR.txt
- <scratch>/synth_work/break_A2.txt
- <scratch>/synth_work/break_B.txt
- <scratch>/synth_work/break_C.txt
- <scratch>/synth_work/fidelity.txt
