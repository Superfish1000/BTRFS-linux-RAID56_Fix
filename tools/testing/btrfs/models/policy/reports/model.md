# model

I built one exhaustive model with every policy as a switch and ran the full matrix. Headline: **A, A2 and CTM never return a silently wrong read in any run. Today's kernel (CUR) does, in five classes.**

**What was run.** The model is `policy_model.py`: `--policy OLD|A|CUR|A2|B|C|CB|CTM`, plus refinement and mutation switches. It reuses the value semantics of `raid56_redundancy_model.py`, the log records and eviction passes of `raid56-wib.c` at stage0-wip 2a1e2379e4, recovery §3.5 from scrub.c, and the stage-1 design of `wf_model.py`.
- 8 policies × 18 scenario families × {nodatasum, csum} = 280 main runs, at depth 7-10 (3.5k to 958k states per run).
- 32 refinement-variant runs, 22 negative controls, 35 re-runs with values that cannot alias, and 30 re-runs checking fixes I made to the model mid-run.

CB is an extra policy: C as §2.1 of the design writes it (phase A never refuses before the trigger).

**Results against the rule "act up rather than lose data":**

1. **CUR is silent in five classes, each after spending or not writing a record.** An alert is raised earlier in every case, but the wrong read itself carries no error.
   - CUR-1: a record naming a missing device is evicted, the device returns, and its stale platter is trusted. A later scrub makes the loss permanent.
   - CUR-2: a recovery verdict is evicted while degraded.
   - CUR-3: tree-log replay evicts records **with every disk present**, because `BTRFS_FS_LOG_RECOVERING` is set.
   - CUR-4: a flush loss the log cannot name is acknowledged (`nr_unnamed`).
   - CUR-5 (RAID6): one missing disk lets the log evict the records naming a present, failing disk.

   Checksums make all of them detected errors: no checksummed run is silent.
2. **A2 closes CUR-1, -2, -3 and -5 with no silent state anywhere.** CUR-4 needs its own fix: abort the commit (read-only) instead of acknowledging.
3. **A's biggest cost is acknowledged data that stays on disk but is unreadable forever** (42k states in `persistent`). A queued repair, or a write into the stale column itself, crashes after the in-flight mark and before writing anything; recovery then marks the stripe undecidable (SCRUB_WIB_TORN). The kernel marks the whole stripe in flight before phase A (raid56.c:4939 before 4977).
   - The NI refinement (mark only the columns phase B writes, after phase A's persist) removes this completely: A+NI has no loss of any kind in `persistent`, and refuses 45% fewer writes.
   - NI is not implemented. I give a UML arm to reproduce the class on today's kernel.
4. **A wedges in two cases A2 and C avoid:**
   - with a failing disk, a full log and no spare disk, A is STUCK;
   - with a full log at tree-log replay, the mount fails and no admin action gets past it.
5. **C (md-style failed disk) keeps writing only by accepting loud data loss:**
   - it re-opens the degraded write hole on the failed disk (18,840 states where the acknowledged data is gone from the disks, against 0 for A);
   - after any crash while degraded, the failed column's nodatasum sectors read EIO for good;
   - ERRATA 2's untainted drop is silent, like CUR-1;
   - a second disk loss leaves it read-only with no way back to writing;
   - metadata written while degraded is lost on a crash, where A goes read-only and loses nothing.

   Made strict (SD+NI), C loses nothing but refuses more writes than A+NI.
6. **B and CB are dominated.** B re-opens the same hole on a present failing disk. CB's degrade in phase A gives 2.6-3x C's gone-from-disk losses.
7. **Every log policy shares the degraded write hole:** a crash while a device is missing. Refusing such writes (SD) closes it, except a crash and a disk loss at the same moment. Only data journalling (PPL-like) would close that.

Everything is in RESULTS.md; tables are in summary.md.

## Table

Cell = SILENT / LOST(gone: value no longer on any disk) / LOST(unreach: on disk, no admin action reads it) / STUCK (distinct states) / REF_WRITE / RO+mount-failed (distinct transitions). k = thousands; Tn = run stopped on its 2.5 GB memory limit after depth n. RAID5 with 2 full stripes unless the family says otherwise; the family's depth is in brackets.

| family (nodatasum) | OLD | A | CUR | A2 | B | C | CB | CTM |
|---|---|---|---|---|---|---|---|---|
| transient (d10) | 230k/156k/48k/0/0/0 | 0/0/55k/0/84k/0 | 0/0/55k/0/84k/0 | 0/0/55k/0/84k/0 | 0/20k/57k/0/78k/0 | 0/18k/149k/0/190k/0 | 0/54k/154k/0/200k/0 | 0/18k/149k/0/190k/0 |
| persistent (d10) | 310k/206k/65k/0/0/0 | 0/0/42k/0/81k/0 | 0/0/42k/0/81k/0 | 0/0/42k/0/81k/0 | 0/19k/45k/0/63k/0 | 0/19k/129k/0/180k/0 | 0/49k/134k/0/174k/0 | 0/19k/129k/0/180k/0 |
| flush (d10) | 258k/170k/53k/0/0/0 | 0/0/149k/0/134k/0 | 0/0/149k/0/134k/0 | 0/0/149k/0/134k/0 | 0/0/149k/0/134k/0 | 0/19k/59k/0/84k/0 | 0/19k/59k/0/84k/0 | 0/19k/59k/0/84k/0 |
| flush_unnamed (d9) | 36k/23k/7384/0/0/0 | 0/0/0/0/4472/4472 | 25k/16k/8494/0/0/0 | 0/0/0/0/4472/4472 | 0/0/0/0/4472/4472 | 0/1472/6008/0/6880/0 | 0/1472/6008/0/6880/0 | 0/1472/6008/0/6880/0 |
| crash_loss (d10) | 35k/35k/0/0/0/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 | 0/14k/27k/0/47k/0 |
| missing_return (d10) | 35k/25k/9680/0/0/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/2544/5520/0/10k/0 | 0/3312/9936/0/13k/0 |
| missing_return_full (d10, cap 1) | 35k/25k/9680/0/0/0 | 0/1312/2912/0/28k/0 | 38k/32k/21k/0/52k/0 | 0/7120/22k/0/98k/0 | 0/1312/2912/0/28k/0 | 24k/21k/19k/0/72k/0 | 24k/21k/19k/0/72k/0 | 0/3312/9936/0/40k/0 |
| missing_return_twice (d9) | 505k/418k/53k/0/0/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/42k/18k/0/61k/0 | 0/6064/18k/0/22k/0 |
| persistent_full (d10, cap 1) | 37k/25k/7606/0/0/0 | 0/0/1408/0/15k/0 | 0/0/1408/0/15k/0 | 0/0/1408/0/15k/0 | 0/448/1408/0/14k/0 | 0/1056/6336/0/24k/0 | 0/1760/6336/0/24k/0 | 0/1056/6336/0/24k/0 |
| replay_full (d9, cap 1) | 16k/11k/256/0/0/0 | 0/0/1536/0/14k/1920 | 1520/756/512/0/18k/384 | 0/0/512/0/14k/1920 | 0/256/1536/0/12k/1536 | 0/760/4076/0/16k/384 | 0/1272/4080/0/16k/0 | 0/760/4076/0/16k/384 |
| replay_full_transient (d9) | 33k/22k/6930/0/0/0 | 0/0/3752/0/27k/3432 | 3192/1568/2576/0/33k/384 | 0/0/2728/0/27k/3432 | 0/512/3756/0/26k/3048 | 0/1472/9720/0/40k/1896 | 0/2488/9748/0/41k/1512 | 0/1472/9720/0/40k/1896 |
| replace (d8, failing + missing disk: a double fault) | 541k/518k/11k/0/380k/0 T7 | 0/109k/38k/0/288k/0 | 0/109k/38k/0/288k/0 | 0/109k/38k/0/288k/0 | 0/121k/39k/0/294k/0 | 0/136k/55k/0/315k/0 | 0/149k/56k/0/322k/0 | 0/133k/53k/0/298k/0 |
| raid5_second_fault (d7) | 380k/364k/7990/0/256k/0 | 0/67k/26k/0/164k/0 | 0/67k/26k/0/164k/0 | 0/67k/26k/0/164k/0 | 0/75k/26k/0/168k/0 | 0/94k/39k/0/174k/0 | 0/103k/40k/0/178k/0 | 0/93k/37k/0/163k/0 |
| nospare_persistent_full (d10) | 16k/11k/256/0/0/0 | 0/0/768/2432/13k/0 | 0/0/768/0/13k/0 | 0/0/768/0/13k/0 | 0/256/768/2432/12k/0 | 0/768/4736/0/18k/0 | 0/1280/4736/0/18k/0 | 0/768/4736/0/18k/0 |
| cap2_missing_return (d7, cap 2, 3 stripes) | 6032/2908/3124/0/0/0 | 0/0/0/0/8028/0 | 1788/576/1212/0/0/0 | 0/0/0/0/288/0 | 0/0/0/0/8028/0 | 1788/576/1212/0/0/0 | 1788/576/1212/0/0/0 | 0/0/0/0/0/0 |
| raid6_second (d8) | 97k/94k/2272/0/0/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/52k/117k/0/133k/0 | 0/47k/104k/0/122k/0 |
| raid6_bad_missing_full (d7) | 443k/199k/113k/0/0/0 T6 | 0/5096/39k/0/274k/0 | 96k/33k/130k/32/146k/0 T6 | 0/8488/84k/0/377k/0 | 0/13k/47k/0/294k/0 | 0/22k/164k/0/236k/0 T6 | 0/31k/168k/0/228k/0 T6 | 0/34k/201k/0/372k/0 |

| family (csum; metadata for meta_*) | OLD | A | CUR | A2 | B | C | CB | CTM |
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

**Refinement variants** (persistent family, nodatasum; format LOST(gone) / LOST(unreach) / REF_WRITE):

| Variant | Result |
|---|---|
| A | 0 / 42,356 / 81,300 |
| A+NI | 0 / 0 / 44,724 |
| C | 18,840 / 129,442 / 180,464 |
| C+NI | 18,848 / 32,832 / 91,460 |
| C+SD+NI (same as CTM+SD+NI) | 0 / 0 / 109,712 |

Full rows, including states, needless read errors and dropped records, are in summary.md.

## Counterexamples

### OLD: SILENT_WRONG (silent corruption (the classic write hole))

Scenario: crash_loss (write hole)

Trace: write s0.d0=2 ; phaseB landed {d0} ; CRASH ; mount without d1  => read s0.d1 = rebuild from P that still describes d0=1 (garbage), no error

Kernel: Upstream behaviour without the log (reference only). With the log, the in-flight record makes recovery treat the stripe as torn: every log policy reads EIO here (§3.5, scrub.c:5385/5122).

### OLD: SILENT_WRONG then LOST(gone) (silent loss of acknowledged nodatasum data)

Scenario: persistent/transient

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed)  => s0.d0 reads 1; add || scrub and P is recomputed from the stale d0, so the value is gone

Kernel: Upstream without the log. Scrub trusts nodatasum (scrub.c:931, 'no other choice but to trust it').

### CUR: SILENT_WRONG (LOST(gone) after a scrub) (silent wrong read; record_dropped alert raised earlier)

Scenario: missing_return_full / cap2_missing_return (CUR-1)

Trace: d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] ok || unmount ; mount with {d0} back  => s0.d0 reads 1 (acknowledged 2); || scrub makes it permanent

Kernel: Applies. wib_may_evict_naming() (raid56-wib.c:716-726) is true on missing_devices (724). wib_evict_sticky() (801), pass loop 847, spends the naming record, reached from wib_find_or_alloc_entry() (982) in btrfs_wib_mark() (3552). On return, device_list_add() clears MISSING with no resync (volumes.c:943). mark_stale_sectors() (raid56.c:2060) finds no record.

### CUR: SILENT_WRONG (silent wrong read, immediately while degraded; alert raised)

Scenario: missing_return_full (CUR-2, verdict eviction, pass 3)

Trace: write s0.d0=2 ; phaseB landed {d0} ; CRASH ; mount without d1 || write s1.d0=2 evict s0TV[2] ok  => s0.d1 reads a rebuild from the torn P (garbage)

Kernel: Applies. The verdict comes from scrub_raid56_recover_absent() (scrub.c:5385) and scrub_raid56_mark_suspect() (5122). wib_evict_sticky() pass 3 (raid56-wib.c:847) spends it. After that, recover_rbio()'s refusal (raid56.c:3337, via btrfs_wib_unrecovered() at raid56-wib.c:4418 and btrfs_wib_stripe_torn()) no longer fires.

### CUR: SILENT_WRONG (silent wrong read with no device missing; alert raised)

Scenario: replay_full / replay_full_transient (CUR-3, every disk present)

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || fsync s1.d0=2 (tree log) ; CRASH ; mount ; replay s1.d0=2 evict s0[0] ok  => s0.d0 reads 1 (acknowledged 2)

Kernel: Applies. BTRFS_FS_LOG_RECOVERING is set at tree-log.c:7755 and tested at raid56-wib.c:725. With a replay pending, error records are only verified (raid56-wib.c:7544) and put back in the live table (7575-7577). The replay's RMW mark (raid56.c:4939) evicts them.

### CUR: SILENT_WRONG (silent wrong read; EV_LOG_UNFLUSHED latched)

Scenario: flush_unnamed (CUR-4)

Trace: d0 starts failing flushes || write s0.d0=2 (flush lost {d0}, log cannot name it: torn-only + alert) ok  => s0.d0 reads 1

Kernel: Applies when the readd plan cannot name the member (wide layout does not fit; raid56-wib.c:3032). wib_readd_dropped() nr_unnamed raises EV_LOG_UNFLUSHED (3195), message 'may read back an older version' (3211), and the commit goes on. The model cannot derive this condition; it switches it on (--unnamed-flush).

### CUR: SILENT_WRONG (+32 STUCK) (silent wrong read; 96k states when the run stopped at depth 6; alert raised)

Scenario: raid6_bad_missing_full (CUR-5)

Trace: write s0.d0=2 ; CRASH after mark ; mount without d0 || d1 starts failing writes || write s0.d1=2 ok (write to {d1} failed) || write s1.d0=2 evict s0[1] ok  => s0.d1 reads 1 (acknowledged 2) from a PRESENT device

Kernel: Applies. wib_may_evict_naming() is one flag for the whole filesystem (raid56-wib.c:716-726), so while any device is missing, pass 1 (847) also spends records naming a present failing device.

### CUR: RO/down (mount fails) (mount failure (loud; data kept))

Scenario: replay_full (384 transitions)

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || fsync s0.d1=2 (tree log) ; CRASH ; mount ; replay s0.d1=2 REFUSED(phaseA {d0} write-back failed)

Kernel: Applies. rmw_repair_first() refuses at raid56.c:4743, so the tree-log replay fails. A has the same class.

### A: LOST_ACKED (unreachable: the data is on disk) (loud loss (EIO forever, data preserved); the largest cost of strict refusal)

Scenario: transient / persistent / flush / replay (A-1). 42k states in persistent; the same class in CUR and A2

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || repair s0 ; CRASH after mark ; mount   (or: ... || write s0.d0=3 ; CRASH after mark ; mount)  => s0.d0 EIO forever although P still holds 2

Kernel: Applies. rmw_rbio() marks the whole full stripe in flight (raid56.c:4939) before rmw_repair_first() (called at 4977); a queued repair (raid56_submit_repair, 4317) does the same. Recovery plans SCRUB_WIB_TORN (scrub.c:3523), then scrub_raid56_mark_suspect() (5122), and reads refuse (raid56.c:3337). torn_present.sh 'named' tests the refusal where it is needed (torn write to another column). The model's NI refinement removes this class; not implemented.

### A: LOST_ACKED (gone) (loud loss (EIO, detected))

Scenario: crash_loss / missing_return* / raid6 (A-2, the degraded write hole; the same class in every log policy)

Trace: d0 detaches || write s0.d0=2 ok || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount  => d0's acknowledged value was only in P, and P is torn

Kernel: Applies. Degraded RMWs are allowed. Recovery keeps the stripe (scrub.c:5767) and classifies it (5385/5122), so reads get EIO, not a guess. This is the documented [LIMITS] of raid56-wib.c. SD closes it except for a crash and a disk loss at the same moment.

### A: REFUSED_WRITE / STUCK (availability (the user's accepted 'act up'))

Scenario: missing_return_full (28k refusals); nospare_persistent_full (STUCK 2,432) (A-3)

Trace: with a device missing, or a persistently failing disk attached, and cap 1: every write into a new region => REFUSED(log-full); without a spare disk no admin action restores writes

Kernel: Kernel only with raid56_keep_naming_degraded=1 (knob at raid56-wib.c:592, tested at 722): btrfs_wib_mark() fails at WIB_ROOM_NONE (3636). The default kernel evicts instead (CUR-1/2).

### A: RO/down, and the fsync'd data unreachable (mount fails; pulling the disk does not help A (it keeps the missing device's names))

Scenario: replay_full (A-4)

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || fsync s1.d0=2 (tree log) ; CRASH ; mount ; replay s1.d0=2 REFUSED(log-full)

Kernel: Applies with keep_naming_degraded=1: btrfs_wib_mark() refuses (raid56-wib.c:3636), or phase A fails (raid56.c:4743), during btrfs_recover_log_trees(). A2 gets past it: pull the disk, then drop plus taint.

### A2: LOST_ACKED (unreachable) (loud; never silent in any run; the A-3/A-4 wedges are removed)

Scenario: same classes as A (A-1, A-2), in more states because A2 keeps writing

Trace: e.g. write s0.d0=2 ; CRASH after mark ; mount without d0 || replace d0  => §3.5 verdict; the replace writes zeros; P held the value

Kernel: Proposed policy (task #76), not in the kernel.

### B: LOST_ACKED (gone) (loud data loss that A avoids by refusing that write)

Scenario: transient / persistent (19k in persistent)

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || write s0.d1=2 phaseA {d0} refused->degrade ; phaseB landed {d1} ; CRASH ; mount  => d0's acknowledged 2 was only in P, now torn

Kernel: Hypothetical policy. The kernel refuses at raid56.c:4743.

### C: LOST_ACKED (gone) (loud loss of acknowledged data, including checksummed metadata (2,032 states), where A goes read-only and loses nothing)

Scenario: persistent / transient / flush / meta_persistent_full (C-1, C-5)

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || trigger fail d0 || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount   (metadata: ... || write s1.d0=2 T-log fails d0 ok || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount)

Kernel: Design (stage 1), not implemented.

### C: LOST_ACKED (unreachable) (loud; NI reduces it to 32,832)

Scenario: every family with a failing disk (129k in persistent; C-2)

Trace: d0 starts failing writes || trigger fail d0 || write s0.d0=2 ; CRASH after mark ; mount  => §3.5 marks the stripe suspect; d0's nodatasum is EIO forever

Kernel: Design §3.5; the same code as scrub_raid56_recover_absent() in the kernel.

### C: SILENT_WRONG (silent, the same as CUR-1; still 11,344 states under SD+NI)

Scenario: missing_return_full / cap2_missing_return (C-3, ERRATA 2)

Trace: d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] ; CRASH after mark ; mount with {d0} back  => s0.d0 reads 1

Kernel: Design ERRATA 2 keeps HEAD's missing-device exception without taint. CTM (T-missing) and A2's taint remove it.

### C: STUCK (read-only for good after a second disk loss; A (failing disk still a member) can replace the missing disk)

Scenario: raid5_second_fault with csum (1,788; C-4)

Trace: d0 starts failing writes || trigger fail d0 || d1 detaches (beyond tolerance: read-only)

Kernel: Design stage 1 (no readmit).

### C (mutation c_trust_unverified): SILENT_WRONG (control: 171,592 states)

Scenario: persistent (negative control)

Trace: a FAILED device's nodatasum sectors served unverified => stale reads

Kernel: n/a (a broken C that shows the model sees the failure)

## Sensitivity

**Negative controls.** 21 of 22 fire (`summary.md`, "negative controls"):

| Control | Metric | Count |
|---|---|---|
| OLD write hole | SILENT | 34,798 |
| OLD stale member | SILENT | 229,816 |
| OLD flush | SILENT | 257,624 |
| OLD csum write hole | LOST(gone) | 13,432 |
| CUR degraded eviction | SILENT | 38,464 |
| CUR replay eviction | SILENT | 1,520 |
| CUR unnamed flush | SILENT | 24,508 |
| B degraded write hole | LOST(gone) | 19,024 |
| C degraded write hole | LOST(gone) | 18,840 |
| Broken C: unverified reads from the FAILED device | SILENT | 171,592 |
| Broken CTM: the same | SILENT | 34,172 |
| A + evict_naming | SILENT | 39,964 |
| A2 without taint | SILENT | 24,320 |
| A + no_suspect (§3.5 off) | SILENT | 34,798 |
| A + absent_par_unnamed | SILENT | 27,920 |
| A + ack_unnamed | SILENT | 24,508 |
| B without its record | SILENT | 169,546 |
| B + read_trust_torn | SILENT | 27,956 |
| A degraded wedge | REFUSED_WRITE | 28,024 |
| A mount fails on replay | RO | 1,920 |
| A without a spare disk | STUCK | 2,432 |

- **The one control that does not fire, A + read_trust_torn (0), is a finding.** Under A, phase B never runs while a present column is named, and flush failures are not concurrent in the model. So the torn-read refusal never protects anything under A here; its only effect is the A-1 cost. The kernel needs it for a concurrent failed flush (`torn_present.sh named`).
- **No-aliasing re-runs.** 35 runs with `--fresh-values` give the same verdicts as the matrix: which policies are silent, and which lose data from the disks.
- **Model fixes during the run.** I fixed three things in the model while the matrix ran:
  - the refused-beyond-tolerance path;
  - A2 serving checksum-verified sectors of a tainted device;
  - RAID6 wrong rebuilds that compared equal and passed the Q cross-check.

  Two things were re-run: every affected A2 run, and every RAID6 run (the RAID6 fix made B's false 4,032 silent states go to 0). 26 early runs re-run with the final model are identical counter for counter (`verify_sample.log`), and so are 4 RAID5 runs after the RAID6 fix (`verify_garbage.log`).

## Caveats

**What the model abstracts:**
- One sector per column, no rotation, 2 data columns, 2 full stripes (3 in `cap2`).
- Log capacity is counted in records; the kernel's narrow/wide layout is not modelled.
- One operation at a time, with every crash point inside each write and each repair.
- Replace and scrub are atomic with respect to writes.
- A write's acknowledgement is its commit (fsync semantics).
- The C trigger is atomic and durable. The FAILED-PENDING window, superblock list, witness rule and readmit are left to `wf_model.py` and its attack runs.
- Per-device log slots are left to `raid56_wib_log_model.py`.
- RAID6 Q is a snapshot, not GF(2^8) arithmetic.
- Torn-only eviction order is any order, a superset of the kernel's.

**Search bounds:**
- Depth 7-10, 1-2 crashes, one write fault, 1-2 device losses.
- LOST and STUCK are judged with an admin closure of at most 3 actions.
- 8 runs stopped on the 2.5 GB memory limit and are marked Tn: OLD in `replace`; OLD, CUR, C and CB in `raid6_bad_missing_full`. Their counts are exact up to that depth.
- Counts are distinct states and depend on how much each policy's state space grows, so compare zero against non-zero and the traces, not raw magnitudes.

**Artifacts to discount:**
- CTM's lower counts in `missing_return_twice` and `raid6_second` come from the device budget (a returned disk stays FAILED, so a further loss would exceed tolerance). The second-fault case is measured only in `raid5_second_fault` (`--overfault`).
- The `replace` family is a double fault on RAID5.

**Not implemented or observed on a kernel:**
- NI and SD are model refinements only. NI's kernel form is inferred (reorder `btrfs_wib_mark()` after phase A's persist, mark only the columns phase B writes, adjust the recovery plan) and not built.
- CUR-4 depends on an environment condition the model switches on but cannot derive.
- None of the counterexamples was reproduced on a UML kernel; RESULTS.md §9 gives the arm for A-1.

**Kernel lines:** cited at BTRFS-s0 stage0-wip 2a1e2379e4, whose fs/btrfs is identical to 7e07215624. The branch moved three times during the work; `kcite.sh` prints the lines at any HEAD.

**Workload:** the whole matrix ran on a shared, loaded machine (load up to 7.7) at nice 10. Some families were reduced by one level of depth; every table shows the depth used.

## Model files

- <scratch>/policy_model.py
- <scratch>/policy_model.md5
- <scratch>/run_all.sh
- <scratch>/RESULTS.md
- <scratch>/summary.md
- <scratch>/summarize.py
- <scratch>/classes.py
- <scratch>/compact.py
- <scratch>/assemble.py
- <scratch>/kcite.sh
- <scratch>/verify_sample.sh
- <scratch>/verify_sample.log
- <scratch>/verify_garbage.sh
- <scratch>/verify_garbage.log
- <scratch>/run_fresh.sh
- <scratch>/fresh_summary.txt
- <scratch>/out/
- <scratch>/out_fresh/
- <scratch>/out_raid6_oldgarbage/
