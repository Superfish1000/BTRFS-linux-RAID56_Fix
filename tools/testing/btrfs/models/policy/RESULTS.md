# RAID5/6 failure policies compared in one exhaustive model

This work tests the policies against the user's rule: **"I would rather the system act up than lose data."** Under that rule:
- refusing work (EIO, read-only, a mount that fails) is acceptable;
- a silently wrong read is not;
- losing acknowledged data is not.

Files are in `<scratch>/`:

| File | What it is |
|---|---|
| `policy_model.py` | The model. The final md5 is in `policy_model.md5`. |
| `run_all.sh` | The matrix, the variant runs and the negative controls. |
| `summarize.py` | Builds `summary.md` (every number below). |
| `classes.py` | Prints the distinct counterexample traces of each run. |
| `verify_sample.sh`, `verify_sample.log`, `verify_garbage.sh`, `verify_garbage.log` | Re-runs of early runs with the final model (§7). |
| `run_fresh.sh`, `fresh_summary.txt`, `out_fresh/` | The no-aliasing cross-check (§6). |
| `compact.py`, `assemble.py`, `kcite.sh` | Build the §3 tables and this file; print the kernel citations. |
| `out/X_*.txt` | Side runs: torn-only eviction, CUR's return class. |
| `out_raid6_oldgarbage/` | RAID6 results before the garbage-tag fix; superseded (§6). |
| `out/*.txt` | One file per run: the RESULT line, the CONFIG line, up to 4 distinct counterexample traces per metric, and the first trace of each transition event. |

Kernel citations are at BTRFS-s0 `stage0-wip` **2a1e2379e4**. Its fs/btrfs is identical to 7e07215624. The branch moved three times while this ran, so cite by function name; `kcite.sh` prints every cited line at the current HEAD.

## 0. Bottom line

The model has 8 policies and 18 scenario families, each run with nodatasum and checksummed data (280 main runs), at depth 7-10. On top of that:
- 32 runs of refinement variants;
- 22 negative controls;
- 35 no-aliasing re-runs;
- 30 re-runs checking the model fixes.

Every reachable state up to the depth was examined: 3.5k to 958k states per run, 51M in all. Each metric has a negative control that fires (§6). Two cross-checks agree:
- re-runs with values that cannot alias;
- re-runs of the early runs with the final model (26 of 26 identical).

**Silent wrong reads.**
- **Never** under A, A2 or CTM, in any family.
- **CUR (today's kernel)** has them in 5 classes, each after a record it spent or never wrote:
  - with a device missing (CUR-1 and CUR-2);
  - **during tree-log replay with every disk present** (CUR-3);
  - when a flush loss cannot be named (CUR-4);
  - on RAID6, where a missing disk also frees the records of a present failing disk (CUR-5).
- **C and CB** as ERRATA 2 writes them have them too (`missing_return_full`, `cap2_missing_return`): the untainted drop of a missing disk's records.
- **OLD** has them everywhere with nodatasum.
- **Checksums** turn every one of these into a detected error: 0 silent with csum for every policy.

**Acknowledged data gone from the disks** (loud, EIO):
- **A, A2:** 0 in every family with one failing but present disk. Losses appear only when a device is missing and a crash tears a stripe: the degraded write hole every policy shares.
- **CUR:** also 0 in those families, except where one of its silent classes becomes permanent (`flush_unnamed`, `replay_full`).
- **B, C, CB, CTM:** they re-open that hole on the failing disk itself: 18,840 (C) to 49,376 (CB) states in `persistent`.

**Acknowledged data on disk but unreadable forever** (loud): the largest cost of A, A2 and CUR.
- Cause: the whole-stripe in-flight mark, set before phase A. A queued repair, or a write into the stale column itself, that crashes before writing anything leaves a stripe no scrub or replace will read again.
- A refinement, NI (§5), removes all of it in the model: A+NI has **no loss of any kind** in `persistent`, and 45% fewer refused writes.

**Wedges.**
- A with a failing disk, a full log and no spare disk is **STUCK** (no writes until a disk arrives).
- A with a full log at tree-log replay fails the mount, and no admin action gets past it.
- A2 and C do not wedge: A2 by pulling the disk and dropping plus tainting its records, C by failing it.

**C, the md-style failed disk, trades loud data loss for write availability.**
- Made strict (SD+NI), it loses nothing in the single-failing-disk families, but refuses more writes than A+NI.
- A second disk loss after the trigger leaves it read-only for good. A can still replace the missing disk.
- It is worth doing for the log and the replay wedge and as an operational state, with phase A refusing until the trigger and with T-missing. It is not a way to keep writing without risk.

## 1. The model

**What is modelled.** RAID5 (3 devices) or RAID6 (4 devices):
- 2 data columns and 1 or 2 parities, one device per column;
- 2 full stripes (3 in the `cap2` family), one sector per column.

**Values, not fault counts** (the value semantics of `raid56_redundancy_model.py`):
- A data cell holds an int.
- A parity holds the *snapshot* of the data vector it was computed from.
- A rebuild is right only if the other cells still equal that snapshot. Otherwise it returns a tagged garbage value.
- RAID6 two-erasure solves use P and Q. They are right only if both snapshots agree.
- `acc[s][i]` is the set of values a read may return:
  - the last acknowledged value;
  - `{old,new}` while a write is unacknowledged, or after a crash interrupted it;
  - after a write the caller was told failed, also the new value (POSIX leaves a failed write's range unspecified).

**Log records.** Records are per full stripe: `(torn, names, verdict)`.
- `names` are column indices: a data column means stale data; a parity means stale parity.
- `torn` means a write into the stripe may have been torn (in flight at a crash, or a flush loss the log could not name).
- `verdict` is the recovery's "undecidable" mark.
- `--cap` is the number of records the log holds.

These follow `raid56-wib.c`: the entry fields `@bitmap`/`@torn`, `@stale`/`@stale_par`, and `@suspect_par`.

**A write** is an RMW followed by its commit (fsync semantics), in this order:
1. RMW read. It rebuilds what the record names, or what is absent. It refuses if the record names more than the parity can rebuild (UNDECIDABLE), or if it would have to guess over a torn stripe.
2. Mark: allocate the stripe's record. When the log is full, apply the policy's eviction rule or refuse (`btrfs_wib_mark`). The log copy rule also applies.
3. Phase A: write back every named present column (FUA), then persist. A policy either refuses when this fails or degrades (`rmw_repair_first`).
4. Phase B: the new data and every parity, each a separate device write.
5. Completion:
   - the tolerance check (failed + missing + failed-state columns ≤ npar);
   - record update (`rmw_update_stale_data` / `_parity`): landed columns are cleared; failed and missing columns are named.
6. Commit barrier. A device whose flush fails loses that op's writes, and the loss is named in the record (`wib_readd_dropped`). `--unnamed-flush` models the kernel's `nr_unnamed` case, where the log cannot name it.
7. Acknowledgement.

**Crash points inside every write and every queued repair:**
- after the mark;
- after phase A, before its persist;
- after **every subset** of phase B's device writes.

**The mount that follows a crash or a remount:**
- It may find a device missing, or a missing device returned.
- It runs mount recovery (`btrfs_wib_recover` → `btrfs_scrub_raid56_full_stripe`, including §3.5 / `scrub_raid56_recover_absent` for an absent data column).
- It may have a tree-log replay write to do (`--replay`). With a replay pending:
  - error records are only verified before the replay (raid56-wib.c:7544), and put back in the live table (7575-7577);
  - the replay then runs with `BTRFS_FS_LOG_RECOVERING` set (tree-log.c:7755).

**Other operations** (each atomic with respect to writes):
- runtime device loss (`--detach`) and return at a mount (`--ret`);
- write faults, transient (`heal`) or persistent;
- flush faults;
- scrub, queued repair (`--repair`), replace (`--replace`);
- clean remount;
- the C trigger (`fail`).

**Reads** follow the kernel's read path:
- **Direct read:** unless the record names the column (nodatasum), or the device is failed or tainted (nodatasum).
- **Checksummed sectors:** verified. On a mismatch, every rebuild combination is tried (RAID6 mirror retries).
- **Nodatasum rebuild with a spare parity:** cross-checked (`recover_verify_q`, EV_READ_PARITY).
- **Nodatasum rebuild with no spare parity:** refused on a torn or verdict stripe (`btrfs_wib_unrecovered` / `btrfs_wib_stripe_torn`; `recover_rbio()`, raid56.c:3278-3340); otherwise returned.
- **OLD:** upstream behaviour. The direct read is trusted; the first rebuild is unverified.

**Search.** Breadth-first search, exhaustive to the given depth:
- the state is hashed;
- the search is deterministic;
- every state is quiescent, and the crash branches are expanded inside each op.

**Admin closure** (for LOST_ACKED and STUCK). A breadth-first search of up to 3 admin macro-actions, memoised:
- heal the disk (transient families only);
- return a missing device;
- pull a failing disk;
- remount;
- scrub;
- replace any bad, failed, missing or tainted device;
- `fail` (the C/CB/CTM trigger);
- rewrite cells whose content is undefined, i.e. hold no acknowledged value.

### Metrics
All are distinct reachable states, except REF_WRITE, RO/down and dropped, which are distinct transitions.

| Metric | Definition |
|---|---|
| **SILENT** | A read returns a value outside `acc` with no error. |
| silent-no-alert | SILENT, and neither `record_dropped` nor `log_unflushed` was ever raised. |
| **LOST(gone)** | No admin sequence makes every acknowledged cell read back correctly, **and** the value is gone from the disks: no direct copy and no parity combination yields it (oracle). |
| **LOST(unreach)** | The same, but the value is still on disk. The policy can never read it back. It is lost to the user, but a more permissive kernel could recover it. |
| **REF_READ** | A read fails although the oracle can recover the value (needless EIO). |
| **STUCK** | No data is lost, but no admin sequence restores full write availability, meaning all of: rw; no failing device left in service; a write to every cell in turn acknowledged. |
| **REF_WRITE** | Writes or replays failed with EIO. |
| **RO/down** | Transitions to read-only (a metadata write refused, or a commit aborted), or to a mount that fails (the replay was refused). |
| **dropped** | Record evictions, which raise the `record_dropped` alert. |

## 2. Policies (switches of one model)

| Switch | What it is |
|---|---|
| `OLD` | Upstream btrfs without the log: no records, no mount recovery. Scrub trusts nodatasum data. A returning device is trusted (`device_list_add()`, volumes.c:943-961). |
| `A` | Strict refusal: A2 without its one allowed drop. It does evict vague records (sticky, nothing named, not torn); the model never creates one. Everything else is kept: naming, torn-only, verdict. Refusals: a full log refuses (EIO; RO for metadata); a failed phase-A write-back refuses; a failed persist refuses; reads the record cannot vouch for fail. A flush loss the log cannot name aborts the commit (RO) instead of acknowledging. In the kernel, this is the stage-0 code with `raid56_keep_naming_degraded=1` (knob at raid56-wib.c:592, tested at 722) and `raid56_wf_torn_unevictable=1`. |
| `CUR` | The stage-0 kernel as it is. It is A, except: `wib_may_evict_naming()` (raid56-wib.c:716-726) is true whenever a device is missing or during tree-log replay, and then `wib_evict_sticky()` (801) spends naming records in pass 1 and verdicts in pass 3. With every device present, torn-only records are spent (`wib_entry_torn_only`, 775). A flush loss it cannot name is acknowledged, with EV_LOG_UNFLUSHED (3195-3212). |
| `A2` | A, plus one allowed drop: a record whose names are **all** on currently missing devices (not torn, not a verdict). The drop persistently taints those devices. A tainted device is treated as stale: nodatasum sectors and parity are never used, checksum-verified sectors are served. A scrub that rewrites the whole device, or a replace, clears the taint. |
| `B` | A, but a phase-A write-back refused by the stripe's own disk does not refuse the write. That column is treated as missing for the stripe, and its record keeps naming it. |
| `C` | Stage 1 of the failed-disk design, built on A: A until the trigger. The trigger (`fail`; also T-flush inside the commit, and T-log when the log is full of a failing disk's names) marks the device FAILED durably and forgets its names. After that, writes skip it quietly, only its checksum-verified sectors are served, and its nodatasum sectors and parity are never used. Replace restores it. With a device **missing** and the log full, it follows ERRATA 2 of the design: records naming only missing devices are dropped with the alert, **without taint**. |
| `CB` | C as §2.1 of the design writes it: phase A never refuses (B's degrade) before the trigger. |
| `CTM` | C plus the design's optional T-missing: a missing device is FAILED at once, so no record ever has to name it, and it is distrusted when it returns. |

### Refinements measured as variants (`--mut`)
- **SD = `strict_degraded`:** refuse an RMW that has to rebuild an absent (missing, failed or tainted) data column holding committed data. No write made while degraded can then open the degraded write hole.
- **NI = `narrow_inflight`:** the in-flight mark covers only the data column phase B writes, and is written together with phase A's persist, just before phase B. Recovery trusts a one-parity rebuild whose holes contain every in-flight column; the result is the old or the new value, both acceptable.

## 3. The matrix

Families (RAID5, 2 full stripes, log capacity = stripes unless given):

| Family | Scenario | Switches |
|---|---|---|
| `transient` | a transient write failure on one device | `--bad transient`, 2 crashes, repair, scrub |
| `persistent` | a persistent write failure, device stays attached | `--bad persistent`, 2 crashes, repair, replace, scrub |
| `flush` | barrier failure; the log names the loss | `--flush`, 2 crashes |
| `flush_unnamed` | barrier failure the log cannot name | `--flush --unnamed-flush` |
| `crash_loss` | crash at every point, then a device lost (the classic write hole) | `--detach 1`, 2 crashes |
| `missing_return` | device missing at runtime or at mount, then returning, with or without a scrub | `--detach 1 --ret` |
| `missing_return_full` | as `missing_return`, log capacity 1 | `--cap 1` |
| `missing_return_twice` | two successive absences with returns | `--detach 2 --ret` |
| `cap2_missing_return` | log capacity 2 with 3 full stripes | `--stripes 3 --cap 2`, no crash |
| `persistent_full` | persistent failure and a full log | `--cap 1` |
| `nospare_persistent_full` | as `persistent_full`; no spare disk in the admin closure | `--no-spare` |
| `replay_full` | tree-log replay at mount with a full log | `--replay --cap 1`, persistent failure |
| `replay_full_transient` | as `replay_full` with a transient failure | `--replay --cap 1` |
| `replace` | replace of a failed or missing device | a failing and a missing disk together: a double fault on RAID5 |
| `raid5_second_fault` | a second device lost after the first failed | `--overfault`: beyond tolerance means ro,degraded |
| `raid6_second` | RAID6, two devices missing | `--detach 2` |
| `raid6_bad_missing_full` | RAID6, one failing device plus one missing, log capacity 1 | |
| `meta_persistent_full` | every write is metadata: checksummed, refusal means RO | `--meta` |

- A cell ending in `Tn` means that run stopped on its memory limit (2.5 GB) after completing depth n, plus part of n+1.
- A dash means the policy was not run in that family.
- **Budget artifact.** In `missing_return_twice` and `raid6_second`, CTM's lower counts come from the model's device budget, not from better protection. Under CTM a returned device stays FAILED, so a further loss would exceed the tolerance, and the model explores that only in `raid5_second_fault` (`--overfault`). C equals A in those two families: no disk fails its writes there.
- The `replace` family combines a failing and a missing disk on RAID5: a double fault. Its large LOST(gone) counts reflect that, for every policy.

### nodatasum

| family (nodatasum) | OLD | A | CUR | A2 | B | C | CB | CTM |
|---|---|---|---|---|---|---|---|---|
| transient (d10) | 230k/156k/48k/0/0/0 | 0/0/55k/0/84k/0 | 0/0/55k/0/84k/0 | 0/0/55k/0/84k/0 | 0/20k/57k/0/78k/0 | 0/18k/149k/0/190k/0 | 0/54k/154k/0/200k/0 | 0/18k/149k/0/190k/0 |
| persistent (d10) | 310k/206k/65k/0/0/0 | 0/0/42k/0/81k/0 | 0/0/42k/0/81k/0 | 0/0/42k/0/81k/0 | 0/19k/45k/0/63k/0 | 0/19k/129k/0/180k/0 | 0/49k/134k/0/174k/0 | 0/19k/129k/0/180k/0 |
| flush (d10) | 258k/170k/53k/0/0/0 | 0/0/149k/0/134k/0 | 0/0/149k/0/134k/0 | 0/0/149k/0/134k/0 | 0/0/149k/0/134k/0 | 0/19k/59k/0/84k/0 | 0/19k/59k/0/84k/0 | 0/19k/59k/0/84k/0 |
| flush_unnamed (d9) | 36k/23k/7384/0/0/0 | 0/0/0/0/4472/4472 | 25k/16k/8494/0/0/0 | 0/0/0/0/4472/4472 | 0/0/0/0/4472/4472 | 0/1472/6008/0/6880/0 | 0/1472/6008/0/6880/0 | 0/1472/6008/0/6880/0 |
| crash_loss (d10) | 35k/35k/0/0/0/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 |
| missing_return (d10) | 35k/25k/9680/0/0/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/3312/9936/0/13k/0 |
| missing_return_full (d10) | 35k/25k/9680/0/0/0 | 0/1312/2912/0/28k/0 | 38k/32k/21k/0/52k/0 | 0/7120/22k/0/98k/0 | 0/1312/2912/0/28k/0 | 24k/21k/19k/0/72k/0 | 24k/21k/19k/0/72k/0 | 0/3312/9936/0/40k/0 |
| missing_return_twice (d9) | 505k/418k/53k/0/0/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/6064/18k/0/22k/0 |
| persistent_full (d10) | 37k/25k/7606/0/0/0 | 0/0/1408/0/15k/0 | 0/0/1408/0/15k/0 | 0/0/1408/0/15k/0 | 0/448/1408/0/14k/0 | 0/1056/6336/0/24k/0 | 0/1760/6336/0/24k/0 | 0/1056/6336/0/24k/0 |
| replay_full (d9) | 16k/11k/256/0/0/0 | 0/0/1536/0/14k/1920 | 1520/756/512/0/18k/384 | 0/0/512/0/14k/1920 | 0/256/1536/0/12k/1536 | 0/760/4076/0/16k/384 | 0/1272/4080/0/16k/0 | 0/760/4076/0/16k/384 |
| replay_full_transient (d9) | 33k/22k/6930/0/0/0 | 0/0/3752/0/27k/3432 | 3192/1568/2576/0/33k/384 | 0/0/2728/0/27k/3432 | 0/512/3756/0/26k/3048 | 0/1472/9720/0/40k/1896 | 0/2488/9748/0/41k/1512 | 0/1472/9720/0/40k/1896 |
| replace (d8) | 541k/518k/11k/0/380k/0 T7 | 0/109k/38k/0/288k/0 | 0/109k/38k/0/288k/0 | 0/109k/38k/0/288k/0 | 0/121k/39k/0/294k/0 | 0/136k/55k/0/315k/0 | 0/149k/56k/0/322k/0 | 0/133k/53k/0/298k/0 |
| raid5_second_fault (d7) | 380k/364k/7990/0/256k/0 | 0/67k/26k/0/164k/0 | 0/67k/26k/0/164k/0 | 0/67k/26k/0/164k/0 | 0/75k/26k/0/168k/0 | 0/94k/39k/0/174k/0 | 0/103k/40k/0/178k/0 | 0/93k/37k/0/163k/0 |
| nospare_persistent_full (d10) | 16k/11k/256/0/0/0 | 0/0/768/2432/13k/0 | 0/0/768/0/13k/0 | 0/0/768/0/13k/0 | 0/256/768/2432/12k/0 | 0/768/4736/0/18k/0 | 0/1280/4736/0/18k/0 | 0/768/4736/0/18k/0 |
| cap2_missing_return (d7) | 6032/2908/3124/0/0/0 | 0/0/0/0/8028/0 | 1788/576/1212/0/0/0 | 0/0/0/0/288/0 | 0/0/0/0/8028/0 | 1788/576/1212/0/0/0 | 1788/576/1212/0/0/0 | 0/0/0/0/0/0 |
| raid6_second (d8) | 97k/94k/2272/0/0/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/47k/104k/0/122k/0 |
| raid6_bad_missing_full (d7) | 443k/199k/113k/0/0/0 T6 | 0/5096/39k/0/274k/0 | 96k/33k/130k/32/146k/0 T6 | 0/8488/84k/0/377k/0 | 0/13k/47k/0/294k/0 | 0/22k/164k/0/236k/0 T6 | 0/31k/168k/0/228k/0 T6 | 0/34k/201k/0/372k/0 |

Cell = SILENT / LOST(gone) / LOST(unreach) / STUCK (distinct states) / REF_WRITE / RO+down (distinct transitions); k = thousands; Tn = the run stopped on its memory limit after completing depth n (and part of n+1).

### checksummed data (metadata for `meta_*`)

| family (csum) | OLD | A | CUR | A2 | B | C | CB | CTM |
|---|---|---|---|---|---|---|---|---|
| transient (d10) | 0/19k/0/0/17k/0 | 0/0/0/0/33k/0 | 0/0/0/0/33k/0 | 0/0/0/0/33k/0 | 0/17k/0/0/16k/0 | 0/14k/0/0/44k/0 | 0/31k/0/0/27k/0 | 0/14k/0/0/44k/0 |
| persistent (d10) | 0/18k/0/0/17k/0 | 0/0/0/0/45k/0 | 0/0/0/0/45k/0 | 0/0/0/0/45k/0 | 0/17k/0/0/15k/0 | 0/15k/0/0/56k/0 | 0/27k/0/0/23k/0 | 0/15k/0/0/56k/0 |
| flush (d10) | 0/28k/0/0/22k/0 | 0/0/0/0/0/0 | 0/0/0/0/0/0 | 0/0/0/0/0/0 | 0/0/0/0/0/0 | 0/16k/0/0/14k/0 | 0/16k/0/0/14k/0 | 0/16k/0/0/14k/0 |
| flush_unnamed (d9) | 0/2320/0/0/1720/0 | 0/0/0/0/4472/4472 | 0/3696/0/0/2552/0 | 0/0/0/0/4472/4472 | 0/0/0/0/4472/4472 | 0/1472/0/0/1296/0 | 0/1472/0/0/1296/0 | 0/1472/0/0/1296/0 |
| crash_loss (d10) | 0/13k/0/0/14k/0 | 0/13k/0/0/13k/0 | 0/13k/0/0/13k/0 | 0/13k/0/0/13k/0 | 0/13k/0/0/13k/0 | 0/13k/0/0/13k/0 | 0/13k/0/0/13k/0 | 0/13k/0/0/13k/0 |
| missing_return (d10) | 0/2928/0/0/3488/0 | 0/2544/0/0/3112/0 | 0/2544/0/0/3112/0 | 0/2544/0/0/3112/0 | 0/2544/0/0/3112/0 | 0/2544/0/0/3112/0 | 0/2544/0/0/3112/0 | 0/3936/0/0/4480/0 |
| missing_return_full (d10) | 0/2928/0/0/3488/0 | 0/1312/0/0/19k/0 | 0/12k/0/0/23k/0 | 0/8960/0/0/28k/0 | 0/1312/0/0/19k/0 | 0/8528/0/0/27k/0 | 0/8528/0/0/27k/0 | 0/3936/0/0/13k/0 |
| missing_return_twice (d9) | 0/21k/0/0/114k/0 | 0/20k/0/0/20k/0 | 0/20k/0/0/20k/0 | 0/20k/0/0/20k/0 | 0/20k/0/0/20k/0 | 0/20k/0/0/20k/0 | 0/20k/0/0/20k/0 | 0/6480/0/0/6832/0 |
| persistent_full (d10) | 0/1056/0/0/1048/0 | 0/0/0/0/11k/0 | 0/0/0/0/11k/0 | 0/0/0/0/11k/0 | 0/448/0/0/11k/0 | 0/1056/0/0/5320/0 | 0/1312/0/0/3912/0 | 0/1056/0/0/5320/0 |
| replay_full (d9) | 0/760/736/0/1804/1092 | 0/0/1024/0/13k/1920 | 0/0/0/0/16k/384 | 0/0/0/0/13k/1920 | 0/256/1024/0/11k/1536 | 0/760/0/0/4676/384 | 0/1016/0/0/2904/0 | 0/760/0/0/4676/384 |
| replay_full_transient (d9) | 0/1592/1488/0/3564/2180 | 0/0/2036/0/23k/3432 | 0/0/0/0/28k/384 | 0/0/1012/0/23k/3432 | 0/512/2036/0/22k/3048 | 0/1472/1012/0/17k/1896 | 0/1984/1012/0/15k/1512 | 0/1472/1012/0/17k/1896 |
| replace (d8) | 0/354k/0/0/563k/0 | 0/101k/0/0/243k/0 | 0/101k/0/0/243k/0 | 0/101k/0/0/243k/0 | 0/113k/0/0/248k/0 | 0/122k/176/0/254k/0 | 0/133k/176/0/259k/0 | 0/120k/176/0/239k/0 |
| raid5_second_fault (d7) | 0/172k/0/0/247k/0 | 0/64k/0/0/140k/0 | 0/64k/0/0/140k/0 | 0/64k/0/0/140k/0 | 0/71k/0/0/143k/0 | 0/86k/1000/1788/143k/0 | 0/93k/1000/1788/146k/0 | 0/84k/1000/1788/133k/0 |
| nospare_persistent_full (d10) | 0/768/0/0/760/0 | 0/0/0/2688/11k/0 | 0/0/0/0/11k/0 | 0/0/0/0/11k/0 | 0/256/0/2688/9976/0 | 0/768/0/0/4456/0 | 0/1024/0/0/3048/0 | 0/768/0/0/4456/0 |
| cap2_missing_return (d7) | 0/0/0/0/0/0 | 0/0/0/0/8028/0 | 0/0/0/0/0/0 | 0/0/0/0/0/0 | 0/0/0/0/8028/0 | 0/0/0/0/0/0 | 0/0/0/0/0/0 | 0/0/0/0/0/0 |
| meta_persistent_full (d9) | 0/2032/0/0/984/984 | 0/0/0/0/11k/11k | 0/0/0/0/11k/11k | 0/0/0/0/11k/11k | 0/896/0/0/10k/10k | 0/2032/0/0/5108/5108 | 0/2544/0/0/3720/3720 | 0/2032/0/0/5108/5108 |
| raid6_second (d8) | 0/47k/0/0/40k/0 | 0/77k/0/0/66k/0 | 0/77k/0/0/66k/0 | 0/77k/0/0/66k/0 | 0/77k/0/0/66k/0 | 0/77k/0/0/66k/0 | 0/77k/0/0/66k/0 | 0/69k/0/0/61k/0 |
| raid6_bad_missing_full (d7) | 0/36k/0/0/18k/0 | 0/5384/0/0/259k/0 | 0/22k/0/0/75k/0 T6 | 0/9176/0/0/302k/0 | 0/11k/0/0/255k/0 | 0/27k/18k/0/68k/0 T6 | 0/32k/17k/0/46k/0 T6 | 0/33k/18k/0/92k/0 |

Cell = SILENT / LOST(gone) / LOST(unreach) / STUCK (distinct states) / REF_WRITE / RO+down (distinct transitions); k = thousands; Tn = the run stopped on its memory limit after completing depth n (and part of n+1).

The full per-run table, with states, REF_READ and dropped, is in `summary.md`.

## 4. What breaks each policy: counterexample classes, shortest traces

**How to read a trace.** Steps are separated by `||`. Inside one write, `;` separates its stages. `s0.d0=2` is a write of value 2 to data column 0 of full stripe 0.

**Kernel citations** are at BTRFS-s0 **2a1e2379e4**; its fs/btrfs is identical to 7e07215624. Print them at any HEAD with `kcite.sh`. The fs/btrfs tree had no uncommitted changes when read.

### OLD (baseline: no log)
- **O1, the write hole.** `write s0.d0=2 ; phaseB landed {d0} ; CRASH ; mount without d1`. A read of s0.d1 returns a rebuild from a P that still describes d0=1: garbage, with no error. In `crash_loss` with csum, the same state becomes LOST(gone).
- **O2, stale member.** `d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed)`. The write is acknowledged within the tolerance and nothing records it. s0.d0 then reads 1 with no error.
- **O3, scrub makes it permanent.** O2 then `|| scrub`. Scrub trusts nodatasum (scrub.c:931, "no other choice but to trust it") and recomputes P from the stale d0. The acknowledged value 2 is gone.

### CUR (the stage-0 kernel): every silent class is a record it spends or never writes
Each trace below is the shortest the model found for its class. Each class comes with an alert, which is why silent-no-alert is 0 for CUR. **The read that returns the wrong value itself carries no error.**

**CUR-1: degraded eviction, then the device returns** (`missing_return_full`, `cap2_missing_return`).
- Trace: `d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] ok || unmount ; mount with {d0} back`. s0.d0 then reads 1 (acknowledged: 2).
- Add `|| scrub` and P is regenerated from the stale d0: the value is gone for good (LOST(gone), still silent). The same holds for C, which follows ERRATA 2.
- Kernel path:
  1. `wib_may_evict_naming()` (raid56-wib.c:716) is true on `missing_devices` (724).
  2. `wib_evict_sticky()` (801) spends the naming record in pass 1 (loop at 847), reached from `wib_find_or_alloc_entry()` (982) in `btrfs_wib_mark()` (3552).
  3. On return, `device_list_add()` clears MISSING with no resync (volumes.c:943, "missing devid %llu re-appeared").
  4. `mark_stale_sectors()` (raid56.c:2060) finds no record, and the platter is trusted.
- **Applies.**

**CUR-2: a verdict evicted while degraded** (`missing_return_full`, pass 3).
- Trace: `write s0.d0=2 ; phaseB landed {d0} ; CRASH ; mount without d1 || write s1.d0=2 evict s0TV[2] ok`. s0.d1 then reads a rebuild from the torn P (garbage) at once. No return is needed.
- Kernel path:
  1. The mount's verdict comes from `scrub_raid56_recover_absent()` (scrub.c:5385) and `scrub_raid56_mark_suspect()` (5122).
  2. `wib_evict_sticky()` pass 3 spends it (raid56-wib.c:847).
  3. With the record gone, the read refusal in `recover_rbio()` (raid56.c:3337, via `btrfs_wib_unrecovered()` at raid56-wib.c:4418 and `btrfs_wib_stripe_torn()`) no longer fires.
- **Applies.** The kernel's own warning text in `wib_evict_sticky()` says so.

**CUR-3: tree-log replay evicts with every device present** (`replay_full`).
- Trace: `d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || fsync s1.d0=2 (tree log) ; CRASH ; mount ; replay s1.d0=2 evict s0[0] ok`. s0.d0 then reads 1 (acknowledged: 2).
- Kernel path:
  1. `btrfs_recover_log_trees()` sets `BTRFS_FS_LOG_RECOVERING` (tree-log.c:7755). `wib_may_evict_naming()` tests it at raid56-wib.c:725.
  2. With a replay pending, the mount's error records are only verified (raid56-wib.c:7544). They are put back in the live table (7575-7577) and wait for the replay.
  3. The replay's RMW mark (raid56.c:4939) spends one. `btrfs_wib_recover_after_replay()` never sees it.
- **Applies.** The disk is present and failing, and nothing is missing. This is the one CUR class that does not need a degraded mount.

**CUR-4: a flush loss the log cannot name is acknowledged** (`flush_unnamed`).
- Trace: `d0 starts failing flushes || write s0.d0=2 (flush lost {d0}, log cannot name it: torn-only + alert) ok`. s0.d0 then reads 1.
- Kernel: `wib_readd_dropped()` with `nr_unnamed` raises `EV_LOG_UNFLUSHED` (raid56-wib.c:3195), with the message "data without checksums there may read back an older version" (3211). The commit goes on.
- **Applies** only when the readd's plan cannot name: the wide layout the names need does not fit the records it has to keep (`name = plan == WIB_READD_NAMED || ...`, raid56-wib.c:3032). The model does not track layouts, so it treats that condition as an environment switch.

**CUR-5: RAID6, one device missing plus a failing present device** (`raid6_bad_missing_full`).
- Trace: `write s0.d0=2 ; CRASH after mark ; mount without d0 || d1 starts failing writes || write s0.d1=2 ok (write to {d1} failed) || write s1.d0=2 evict s0[1] ok`. s0.d1 then reads 1 (acknowledged: 2), from a device that is **present**.
- `wib_may_evict_naming()` is one flag for the whole filesystem. While any device is missing, pass 1 also spends records naming a column on a present, failing device, and its stale platter is then read directly.
- Counts: 96k silent states when the run stopped at depth 6.
- A few STUCK states (32) follow the same kind of eviction: a spent record naming a stale P leaves a stripe no admin sequence of three actions can make writable again.
- **Applies** (same functions).

**CUR, not silent: a failed mount** (`replay_full`, RO/down=384).
- A replay write into a stripe whose named column is on the still-failing disk is refused by `rmw_repair_first()` (raid56.c:4743). The replay fails, so the mount fails. This is shared with A.

### A (strict refusal): never silent, and three loud costs
**A-1: acknowledged data on disk that no action reads back** (LOST(unreach); every family with a failing disk).
- Traces:
  - `d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || repair s0 ; CRASH after mark ; mount`: a queued repair crashes before writing anything.
  - `... || write s0.d0=3 ; CRASH after mark ; mount`: the torn write targets the named column itself.
- In both, P still describes the acknowledged value. But the whole-stripe in-flight mark makes recovery plan `SCRUB_WIB_TORN` (scrub.c:3523) for the named nodatasum column, then `scrub_raid56_mark_suspect()` (5122). From then on every read is refused (raid56.c:3337), and no scrub or replace can decide the stripe.
- Kernel cause: `rmw_rbio()` marks the full stripe in flight (raid56.c:4939) **before** `rmw_repair_first()` (called at 4977). A queued repair (`raid56_submit_repair()`, 4317) is an RMW with the same mark.
- **Applies.** The refusal is the intended answer in the kernel's `torn_present.sh named` arm, where the torn write went to another column and the rebuild really would be a guess. The model shows two sub-cases where it is not a guess, and the NI refinement makes exactly those readable (§5).

**A-2: the degraded write hole** (LOST(gone); `crash_loss`, `missing_return*`, raid6 families). The same class appears in every log policy.
- Trace: `d0 detaches || write s0.d0=2 ok || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount`. d0's acknowledged value lived only in P, and the crash tore P.
- Kernel: degraded RMWs are allowed. Recovery keeps the stripe (scrub.c:5767) and classifies it (5385/5122), so the read gets EIO, not a guess.
- **Applies.** It is the documented [LIMITS] of raid56-wib.c. Only journalling the absent column's data, or refusing such writes (SD, §5), closes it.

**A-3: wedged while degraded** (`missing_return_full` REF_WRITE; `nospare_persistent_full` STUCK).
- While a device is missing, every write into a new region fails once the log is full of names of the missing device.
- Kernel: this is the kernel with `raid56_keep_naming_degraded=1` (raid56-wib.c:592). `btrfs_wib_mark()` fails at `WIB_ROOM_NONE` (3636). The default kernel (CUR) evicts instead, which is CUR-1/CUR-2.
- With a persistently failing disk still attached and a full log, A refuses every new-region write until a replace. With no spare disk, that is STUCK (the no-spare runs in §3).

**A-4: the mount fails** (`replay_full`: RO/down; the fsync'd data is LOST(unreach)).
- A replay write into a new region when the log is full of the failing disk's names; or a replay into the named stripe, where the phase-A write-back fails (raid56.c:4743).
- Pulling the disk does not help A: its names are now of a missing device, and A keeps them. The fsync'd data stays in the tree log, and only `zero-log` or a more permissive kernel gets past it.
- A2 does get past it: pull the disk, drop plus taint, the replay proceeds, then replace.

### A2
- **Silent:** never, in any run.
- **Compared with A:**
  - it removes A-3 and A-4's wedge (pull, then mount degraded);
  - its taint makes the returned disk's nodatasum sectors unreadable until a full scrub or replace.
- **Remaining losses** are A-1 and A-2, in more states than A because A2 keeps accepting writes where A refused them.

### B
- **Silent:** never.
- **Loss:** a present failing disk re-opens the degraded write hole (LOST(gone)).
- Trace: `d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || write s0.d1=2 phaseA {d0} refused->degrade ; phaseB landed {d1} ; CRASH ; mount`. d0's acknowledged 2 was only in P, and P is torn.
- A refuses exactly that write, and never loses it.

### C (stage 1 of the design)
- **C-1: the degraded write hole on the failed disk** (LOST(gone)). Trace: `d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || trigger fail d0 || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount`. The rule refuses this; running degraded is exactly what exposes it.
- **C-2: every crash while degraded makes the failed column's nodatasum unreadable** (LOST(unreach), the largest count of any policy). Trace: `d0 starts failing writes || trigger fail d0 || write s0.d0=2 ; CRASH after mark ; mount`. Nothing was written, but §3.5 must call the stripe suspect. NI makes the case where the interrupted write targeted the failed column readable.
- **C-3: silent after ERRATA 2's untainted drop** (`missing_return_full`). Trace: `d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] ; CRASH after mark ; mount with {d0} back`. s0.d0 reads 1. This is CUR-1 again: the design keeps HEAD's missing-device exception without A2's taint. CTM (T-missing) and A2's taint both close it.
- **C-4: a second failure while degraded** (`raid5_second_fault`).
  - C has kept the failed disk out of every read of nodatasum and parity, so a second device loss leaves those sectors EIO.
  - With csum the data stays readable, but the filesystem is read-only with no way back to writing: STUCK 1,788. Trace: `d0 starts failing writes || trigger fail d0 || d1 detaches (beyond tolerance: read-only)`.
  - A still has the failing disk as a readable member, stays within the tolerance, and can replace the missing disk: STUCK 0.
- **C-5: metadata** (`meta_persistent_full`). Trace: `d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || write s1.d0=2 T-log fails d0 ok || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount`. d0's acknowledged tree block is gone: checksummed, so it fails with an error, but it is lost. A goes read-only at the second write instead (`write s0.d1=2 REFUSED(phaseA {d0} write-back failed)`), and loses nothing.

### CB (the design's §2.1: phase A never refuses before the trigger)
C's classes, plus B's before the trigger. It has the most LOST(gone) among the log policies in the single-failure families.

### CTM
- **Silent:** never.
- **Losses:** C-1 and C-2 remain. T-missing removes C-3.
- **Unmodelled cost:** a returned disk stays FAILED until it is replaced, so a second loss in that window is a double fault. The model counts that only in `raid5_second_fault`, via `--overfault`.

## 5. The two refinements (nodatasum; full rows in `summary.md`)

| Family | Run | LOST(gone) | LOST(unreach) | REF_WRITE | RO/down |
|---|---|---|---|---|---|
| persistent (d10) | A | 0 | 42,356 | 81,300 | 0 |
| | **A+NI** | **0** | **0** | 44,724 | 0 |
| | A2+SD+NI | 0 | 0 | 44,724 | 0 |
| | C | 18,840 | 129,442 | 180,464 | 0 |
| | C+NI | 18,848 | 32,832 | 91,460 | 0 |
| | **C+SD+NI** (= CTM+SD+NI) | **0** | **0** | 109,712 | 0 |
| missing_return_full (d10) | A | 1,312 | 2,912 | 28,024 | 0 |
| | A+SD+NI | 192 | 192 | 22,456 | 0 |
| | A2+SD+NI | 192 | 192 | 61,132 | 0 |
| | CTM+SD+NI | 384 | 640 | 31,860 | 0 |
| | C+SD+NI (still ERRATA 2) | 6,096 | 5,632 | 29,464 | 0 (**SILENT 11,344**) |
| crash_loss (d10) | A (= every log policy) | 14,200 | 26,600 | 46,748 | 0 |
| | A+SD+NI (= A2, C, CTM with SD+NI) | 4,288 | 4,480 | 78,208 | 0 |
| replay_full (d9) | A | 0 | 1,536 | 13,928 | 1,920 |
| | A+NI | 0 | 1,024 | 13,172 | 1,920 |
| | A2+SD+NI | 0 | 512 | 13,172 | 1,920 |

### What the refinements do
**NI removes A's loud-loss class, A-1, completely.**
- In `persistent`, A+NI has no loss of any kind.
- It refuses 45% fewer writes than A, because a stripe whose record is no longer "possibly torn" can be repaired and written again.
- Refinement for the kernel:
  - call `btrfs_wib_mark()` after `rmw_repair_first()`'s persist, not before it (raid56.c:4939 vs 4977);
  - mark only the blocks, i.e. data columns, that phase B writes;
  - in `scrub_raid56_plan_wib()` / `wib_recover_one()`, decide a one-parity rebuild as safe when every in-flight block is among the columns being rebuilt.
- The on-disk bitmap is already per 64 KiB block, so no format change should be needed. This is an inference; it was not built.
- `torn_present.sh named` stays refused under NI: there, the in-flight column is not the named one.

**SD (strict_degraded) removes the degraded write hole**, except where the device is lost at the same moment as the crash (`crash_loss`: 4,288 remain for every policy). Only journalling the column's data closes that residual.

**C made strict is worse than A for writes.** C+SD+NI is loss-free in `persistent`, but refuses more writes than A+NI (109,712 vs 44,724). Once the disk is FAILED, every sub-stripe write into a stripe with its column must rebuild that column, and SD refuses exactly those. So the only thing C buys over A is the writes it lets through, and those are exactly the ones that open the degraded write hole.

**ERRATA 2 stays silent even under SD+NI.** C+SD+NI still has 11,344 silent states in `missing_return_full`. The drop without taint is independent of the write path. CTM and A2 have none.

## 6. Sensitivity: every failure class is visible

### Negative controls
Each must be > 0 (see `summary.md`, "negative controls"). All bite, except one explained below.

| Control | Metric | Count |
|---|---|---|
| OLD write hole | SILENT | 34,798 |
| OLD stale member | SILENT | 229,816 |
| OLD flush | SILENT | 257,624 |
| OLD csum write hole | LOST(gone) | 13,432 |
| CUR degraded eviction | SILENT | 38,464 |
| CUR replay eviction | SILENT | 1,520 |
| CUR unnamed flush | SILENT | 24,508 |
| B degraded hole | LOST(gone) | 19,024 |
| C degraded hole | LOST(gone) | 18,840 |
| C trusting unverified F reads | SILENT | 171,592 |
| CTM trusting unverified F reads | SILENT | 34,172 |
| A + evict_naming | SILENT | 39,964 |
| A2 without taint | SILENT | 24,320 |
| A + no_suspect (§3.5 off) | SILENT | 34,798 |
| A + absent_par_unnamed | SILENT | 27,920 |
| A + ack_unnamed | SILENT | 24,508 |
| B without its record | SILENT | 169,546 |
| B + read_trust_torn | SILENT | 27,956 |
| A wedge while degraded | REFUSED_WRITE | 28,024 |
| A mount fails on replay | RO | 1,920 |
| A without a spare disk | STUCK | 2,432 |

**The exception: `K_a_read_trust_torn` = 0.**
- Under A, phase B never runs while a present column is named. Flush failures in the model are not concurrent with other writes. So under A, a torn stripe with a named column always still has an untouched P.
- The torn-read refusal therefore never protects anything under A in this model. Its only effect is the A-1 cost.
- The kernel needs it for a flush that fails while another write is in flight (`torn_present.sh named`). This model does not interleave those.
- The same mutation under B bites (27,956).

### Fresh values
`fresh_summary.txt` covers 35 runs:
- families: transient, persistent, flush_unnamed, crash_loss, missing_return_full, replay_full, raid6_bad_missing_full;
- policies: A, CUR, A2, C, CTM (plus OLD and B for transient).

Every verdict is unchanged:
- **SILENT > 0** exactly for OLD, CUR (flush_unnamed, missing_return_full, replay_full, raid6_bad_missing_full), and C (missing_return_full).
- **LOST(gone) = 0** for A, A2 and CUR in transient, persistent and replay_full.
- **LOST(gone) > 0** for C and CTM in the same families.

### Garbage tags
A RAID6 rebuild that is wrong now differs per parity set, as it does in real P/Q arithmetic. Before this fix, two wrong rebuilds compared equal and passed the Q cross-check. That produced false silent reads for B in RAID6 (4,032). Those results are kept in `out_raid6_oldgarbage/`, and every RAID6 run was re-run: B's silent count is now 0. RAID5 is a relabelling: `verify_garbage.log` shows 4 of 4 SAME.

## 7. Model changes during the run, and how the earlier runs were checked

| Change | Effect | How it was handled |
|---|---|---|
| (a) A write refused beyond the tolerance whose parity landed: its new value is now acceptable (POSIX), and a flush loss the log cannot name is kept as torn, not dropped | Reachable only with two simultaneous faults on RAID5 (a failing disk plus a missing one, or a flush fault plus a missing one) | Found by a side run where A showed silent reads. That run is kept in `out/X_torn_only_evict_A.txt` (after the fix: 0 silent). |
| (b) A2's taint serves checksum-verified sectors (it previously treated them as missing) | Changes A2 runs only | Every A2 run that dropped a record was re-run. |
| (c) `--overfault`, `--no-spare`, `--fresh-values` | Default off; no effect on the other runs | — |
| (d) A memory guard: a run stops at 2.5 GB | Affected runs are marked `Tn` in §3 | 8 runs stopped this way: OLD in `replace`, and CUR/C/CB/OLD in `raid6_bad_missing_full`. |
| (e) A wrong rebuild is tagged by its parity set (see §6, "Garbage tags") | Changes RAID6 only | Every RAID6 run was re-run. `verify_garbage.log`: 4 of 4 RAID5 runs SAME. |
| (f) Families reduced for time and memory on the shared machine (load up to 7.7) | `replace`: `--crash 2 --depth 9` to `--crash 1 --depth 8`, because the first attempt used 4-5 GB per run. `raid6_bad_missing_full`, `cap2_missing_return` and `raid5_second_fault` went from depth 8 to 7. Variants were run on 4 families. | The depth used is shown in every table. |

**Check of the earlier runs.** `verify_sample.sh` re-ran 26 runs that finished before (a)-(c) with the final model, covering every early family. All 26 are SAME, counter for counter (`verify_sample.log`).

**Robustness check, `run_fresh.sh`.** It re-runs the key families with `--fresh-values`: a new value never equals one its column still holds on disk or in any parity snapshot, so a stale copy can never pass for the new data. Results are in `fresh_summary.txt` (§6).

## 8. What the model does not cover (fidelity)

**Geometry and granularity**
- One sector per column and no rotation. All devices can fail, so every role is covered.
- The kernel names at 64 KiB column granularity and tracks sub-column writes (`supplied_all` in `rmw_update_stale_data`); not modelled.
- Capacity is counted in records. The kernel's narrow/wide layout (165 or 82 regions) and the halving when the first stale bit appears are not modelled. That halving is how the kernel reaches `nr_unnamed` (CUR-4), which this model can only switch on.
- Capacities 1 and 2 were run (2 or 3 stripes). The eviction choice is the same at any capacity: it happens whenever the log is full.

**Concurrency**
- One operation at a time. Crash points inside each op are enumerated.
- Races between rbios, scrub and commits are not modelled; `raid56_repair_protocol_model.py` and `wf_model.py` cover them.
- Replace is atomic with respect to writes. A crash during a replace is a crash followed by a replace after the mount.

**The C trigger**
- The trigger is atomic and durable. The FAILED-PENDING window, the superblock list, the witness rule and readmit are not modelled; `wf_model.py` and its attack runs (`design-model-result.md`) cover them.
- T-log is synchronous. T-repair and T-admin are an adversarial `fail` action available at any time.

**Log and recovery details**
- Per-device log slots and torn log blocks are not modelled; `raid56_wib_log_model.py` covers them.
- Torn-only eviction order is any order. The kernel spends the recovery's kept torn records after other torn-only records (`wib_entry_kept_torn`), so the model is a superset there.
- Replace-owned marks (pass 2) are not modelled. Replace is atomic, so no write can evict its marks.

**Mount and replay**
- A read-only mount without recovery (`btrfs_wib_unrecovered` before any recovery) is modelled only for mounts beyond the tolerance.
- The tree-log replay is one pending write.

**Search bounds**
- Bounded depth (7-10 steps).
- At most 1 or 2 crashes, 1 write fault, 1 or 2 device losses, and 1-3 remounts per run.
- LOST and STUCK use an admin closure of at most 3 macro-actions.

**Value domain**
- Values are small integers. Without `--fresh-values`, a new value can equal an old one still held by a stale parity. That can move a state between LOST(gone) and LOST(unreach). §6 shows that it changes no verdict.

**Parity arithmetic**
- Parity is modelled as a snapshot. RAID6 Q is not GF(2^8) arithmetic: a mixed P/Q solve is simply "garbage". `syn_stripe_model.py` (design/synthesis) did the arithmetic.

**Metadata**
- `--meta` makes every write metadata: checksummed, and a refusal means RO.
- Mirrored metadata profiles, and metadata reads with transid 0, are not modelled.

## 9. What this means for the decision (measured, not decided)

Under the rule "act up rather than lose data", the policies rank as follows.

**1. CUR, as it is today, breaks the rule** in five modelled classes, CUR-1 to CUR-5. In each one, a read returns stale or garbage data with no error on that read. In each one an alert (`record_dropped` or `log_unflushed`) was raised earlier, so the corruption is flagged but the bad read itself is not refused.
- CUR-3 (replay) happens with every device present.
- CUR-5 (RAID6) evicts the name of a present, failing disk.

A, A2 and CTM are never silent in any family of the matrix, nor in the fresh-values or RAID6 re-runs.

**2. A2 is the change that meets the rule for stage 0.**
- It fixes CUR-1, CUR-2 and CUR-5, because it drops only records whose names are all on missing devices, and taints those devices.
- It fixes CUR-3 by refusing the replay. The mount then fails until the admin pulls the failing disk; the drop plus taint then lets the replay proceed.
- CUR-4 needs a separate change: abort the commit (read-only) instead of acknowledging a flush loss the log cannot name (raid56-wib.c:3195-3211).
- A2 keeps A's cost: refused writes, RO for metadata, and a mount that fails on a full log. It removes A's worst availability case: STUCK without a spare, and a replay that no admin action gets past.

**3. NI should go with A2.** It is the only change that turns A's remaining loud loss (A-1) into readable data, with no silent cost in any run. It is a kernel refinement (§5) and is **not** implemented. Before relying on it, a UML arm should reproduce A-1:
1. dm-flakey `error_writes` on one data device;
2. a nodatasum write whose column on that device fails, so the log names it;
3. arm `raid56_crash_point=1` for the next write into **that same column**. Crash point 1 holds back the P/Q writes and panics after phase B (raid56.c:1817 and 5013); the column's own data write fails on the dm device;
4. mount read-write with every device present.

   Expected on today's kernel:
   - the column reads EIO for good (`torn_undecidable`), while P still holds the acknowledged value;
   - scrub and replace cannot change that.

   This differs from `torn_present.sh named`, where the torn write went to another column.

**4. C, the md-style failed disk, keeps writing only by accepting loud data loss.** Measured costs, nodatasum:
- **C-1:** the degraded write hole on the failed disk, 18,840 LOST(gone) states in `persistent`. A has 0.
- **C-2:** every crash while degraded makes that column's nodatasum unreadable. 129,442 LOST(unreach); 32,832 with NI.
- **C-3:** ERRATA 2's untainted drop is silent (24,320 in `missing_return_full`). Use T-missing or A2's taint instead.
- **C-4:** a second disk loss after the trigger leaves the filesystem read-only with no way back to writing (STUCK in `raid5_second_fault` with csum). A, where the failing disk is still a readable member, can still replace the missing disk.
- **Metadata:** checksummed metadata written while degraded is lost on a crash (2,032 LOST(gone) in `meta_persistent_full`). A goes RO and loses none.

If C must also satisfy the rule, it has to refuse the writes that would open the hole (C+SD+NI: loss-free in `persistent`), and then it refuses more writes than A+NI. So C is only worth building as:
- a way to **stop the log filling** (T-log);
- a way to **get past the replay/wedge** (see A2);
- a **clear operational state** ("failed, replace it");

with phase A refusing until the trigger (C, not CB: CB's phase-A degrade gives 2.6-3x C's LOST(gone) in `persistent` and `transient`), and T-missing (CTM) instead of ERRATA 2.

Closing the degraded write hole itself needs data journalling of the absent column: a partial-parity log, as md's PPL does. That is outside every policy modelled here.

**5. B is dominated.**
- It has C's degraded-write-hole cost on a present failing disk (LOST(gone) 19,024 in `persistent`), but none of C's operational state.
- Every B loss is a write that A refused.
- With no spare disk it is STUCK like A (2,432): its degrade keeps writing, but the failing disk stays in service.
