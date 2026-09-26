# fidelity

**The harness refused `fidelity/REPORT.md`** ("subagents should return findings as text"). The full report is therefore in this output.
- Everything else is on disk in `<scratch>/fidelity/`.
- Kernel: `/home/user/BTRFS-s0` stage0-wip at `2a1e2379e4`, clean tree. All `file:line` references are at that commit.

## Verdict
1. **CUR's five silent classes follow real kernel code, and all five survive the corrections.**
   - CUR-1 and CUR-5: `wib_may_evict_naming()` (raid56-wib.c:716-726) and pass 1 of `wib_evict_sticky()` (847-862). A returning device is trusted with no resync (volumes.c:943-961).
   - CUR-2: verdicts are spent in pass 3.
   - CUR-3: `LOG_RECOVERING` is set during the replay and its commit (tree-log.c:7755, 7904, 7908). Error records are only verified and put back live (raid56-wib.c:7544, 7386-7389, 7575-7577).
   - CUR-4: plan UNNAMED at 3014, alert at 3196.
   - Two unmodelled gaps make CUR easier to break in the kernel than in the model: D5 and D6 below.
2. **A and A2 have 0 silent states in every rerun cell.** But the model's A is not "stage-0 plus `raid56_keep_naming_degraded=1` and `raid56_wf_torn_unevictable=1`", which is how RESULTS.md §2 describes it. With those knobs the kernel still does two unsafe things:
   - it acknowledges an unnamed flush loss (CUR-4). The model's control `K_a_ack_unnamed` is exactly that kernel: 24,508 silent states.
   - it forgets stripes under `WIB_READD_LOSE` (raid56-wib.c:3016, 3049). The model has no such path.
   
   So A needs a kernel change too: `wib_readd_dropped()` must fail the commit (read-only) in both plans (D4). RESULTS.md asks for this only for A2 and only for CUR-4.
3. **Three divergences changed the numbers.** Each is fixed in `policy_model_fid.py` (`--fid F1,F2,F3`; `--fid none` reproduces the original exactly). 204 runs were redone: A, CUR and A2 in all 18 families with both data kinds, C and CTM in 9 families, 32 variants, 44 single-fix runs, 12 controls, and 20 original-model runs with a write-acknowledgement count.
   - **F1: phase A also writes back the target column when the record names it.**
     - Kernel: `rmw_prepare_repair()` raid56.c:4155-4173 ("A sector this write supplies is kept too"); `rmw_repair_first()` 4675-4812 writes it back with FUA and persists before phase B. `mark_stale_sectors()` 2160-2212 counts the target for the AMBIGUOUS and TORN refusals.
     - Model: `needed` excludes the target (policy_model.py:523), and so does `colsA` (643).
     - Effect, persistent disk: the model acknowledged writes into a named column on a still-failing disk; the kernel refuses them (raid56.c:4743).
     - Effect, healed disk: the model kept the name through phase B, so a crash there gave `SCRUB_WIB_TORN` and a verdict (EIO for good). In the kernel the name is already durably cleared and the stripe recovers.
     - A-1 (LOST unreach) falls 65% in transient, 67% in persistent, 45% in persistent_full and 35% in flush. It stays real: a crash between the mark (raid56.c:4939) and phase A (4977).
   - **F2: a runtime detach is not a missing device for the RAID5/6 code.**
     - Kernel: `btrfs_remove_bdev()` super.c:2498-2540 sets MISSING and increments `missing_devices`, so CUR may evict. But it leaves `dev->bdev` set, and the RAID5/6 code decides presence by `bdev` (raid56.c:4143, 4088-4096; scrub.c:3441-3446).
     - So a named column on that device is written back in phase A, `btrfs_submit_dev_bio()` fails the write (bio.c:714-720), and the RMW is refused.
     - The model's runtime detach equals missing-at-mount, so it proceeds (`writable()`, policy_model.py:245 and 643).
     - Effect: the degraded write hole now needs a degraded mount. LOST(gone) falls 35-45% in the detach families.
   - **F3: RAID6 recovery with a spare parity, and the verdict for a present named column.**
     - Kernel: the plan returns TORN only when the holes equal the good parities (scrub.c:3504-3523); otherwise it is PROVEN with a Q cross-check. In recovery, TORN records a verdict (scrub.c:5713-5723, 5122).
     - Model: `torn_blocks()` ignores the parity count (policy_model.py:333-346, 880, 968), and no verdict is set on the non-absent path.
     - Effect: CUR's 32 STUCK states in raid6_bad_missing_full disappear, and verdicts are now evicted last, as in the kernel.

## Other divergences (documented, not rerun)
- **D5.** Layout halving (165 to 82 entries once anything is stale), `wib_live_max()` at raid56-wib.c:527-537, and `wib_enforce_capacity_locked()` at 941-950.
  - With a device missing, CUR spends naming records without any write asking for room.
  - With nothing evictable, `btrfs_wib_commit()` returns 0 after "block full, keeping the previous one" (5757-5765).
  - Records are 4 MiB regions (32 full stripes each), not one per stripe.
  - The attack_A agent's probe models this and reports silent states under A; I confirmed the kernel lines only.
- **D6. CUR:** with a tree log pending, a torn stripe that has an absent data column is not classified (`scrub_raid56_recover_absent` is called with classify = `!log_replay_pending`, scrub.c:5656-5659). It is kept as plain @torn (raid56-wib.c:7591), so pass 1 can spend it during the replay, which gives a silent degraded read.
  - The model classifies it before the replay (policy_model.py:848).
  - This is one more path to a silent class for CUR only. No matrix family combines replay and detach.
- **D7.** The kernel's RMW does not refuse the rebuild of an unnamed missing column in a torn stripe (`add` is required, raid56.c:2188; `rmw_read_wait_recover()` at 3619-3658 has no torn check). The model does refuse (policy_model.py:558). This is loud in the kernel.
- **D8.** The kernel keeps @torn after a successful RMW (`btrfs_wib_done()`, 3691-3725, never clears it). The model clears it (738-741).
- **D9.** The kernel records no verdict when there are more unknowns than usable parities (scrub.c:5656); the model does (919-941). Only CUR's eviction order differs.
- **D10.** The kernel writes back an unnamed checksummed column whose checksum fails in phase A (4165-4167); the model does not.
- **D11.** Forced COW on RAID5/6 (inode.c:1880-1920): an in-place cell in the model stands for other rows of the same column. The classes still hold at column and stripe granularity.
- **D12.** CUR-3 and A-4 need metadata on RAID5/6, because the replay writes only metadata. With RAID1 metadata neither class arises.

## Effect on the comparison
- **The qualitative ranking of RESULTS.md §9 stands.**
  - CUR is silent in five classes.
  - A and A2 are never silent, once D4's fix is added.
  - A+NI has no loss of any kind in persistent.
  - C re-opens the degraded write hole on a failing disk: 16,144 gone states against 0 for A.
  - ERRATA 2 is still silent: 19k states for C in missing_return_full.
- **Quoted magnitudes that change:**
  - A-1 in persistent: 42,356 becomes 14,102 states (27.8% of states becomes 13.2%).
  - "A+NI refuses 45% fewer" becomes 38% fewer by raw count, or 16.2% vs 10.9% by refused share.
- **REF_WRITE counts are not comparable across policies.** They are raw transitions over state spaces of different sizes. By refused share (refused / (refused + acknowledged)), C refuses more writes than A in the single-failing-disk families:
  - persistent: C 23.5%, A 16.2%;
  - transient: C 19.9%, A 12.4%.
  
  A refuses more only where the log wedges: missing_return_full (56.1% vs 23.7%) and nospare (26.4% vs 21.0%).

## Table

**Reading the cells:**
- Cell = SILENT / LOST(gone) / LOST(unreach) / STUCK / REF_WRITE / RO+mount-failed.
- The first four are distinct states; the last two are distinct transitions.
- Each cell reads original -> corrected (fixes F1, F2, F3); "(=)" means unchanged.
- Tn = run stopped at depth n (memory limit).
- RAID5 with 2 full stripes unless stated; depths as in `run_all.sh`.

| family (nodatasum) | A | CUR | A2 |
|---|---|---|---|
| transient | 0/0/55k/0/84k/0 -> 0/0/19k/0/64k/0 | 0/0/55k/0/84k/0 -> 0/0/19k/0/64k/0 | same as A |
| persistent | 0/0/42k/0/81k/0 -> 0/0/14k/0/63k/0 | same as A | same as A |
| flush | 0/0/149k/0/134k/0 -> 0/0/98k/0/179k/0 | same as A | same as A |
| flush_unnamed | 0/0/0/0/4472/4472 (=) | 25k/16k/8494/0/0/0 (=) | 0/0/0/0/4472/4472 (=) |
| crash_loss | 0/14k/27k/0/47k/0 -> 0/9272/20k/0/90k/0 | same as A | same as A |
| missing_return | 0/2544/5520/0/10k/0 -> 0/1536/3968/0/21k/0 | same as A | same as A |
| missing_return_full | 0/1312/2912/0/28k/0 -> 0/768/2048/0/36k/0 | 38k/32k/21k/0/52k/0 -> 34k/28k/19k/0/67k/0 | 0/7120/22k/0/98k/0 -> 0/6576/21k/0/123k/0 |
| missing_return_twice | 0/42k/18k/0/61k/0 -> 0/23k/11k/0/87k/0 | same as A | same as A |
| persistent_full | 0/0/1408/0/15k/0 -> 0/0/768/0/12k/0 | same as A | same as A |
| replay_full | 0/0/1536/0/14k/1920 -> 0/0/1024/0/11k/1536 | 1520/756/512/0/18k/384 -> 512/256/256/0/14k/384 | 0/0/512/0/14k/1920 -> 0/0/256/0/11k/1536 |
| replay_full_transient | 0/0/3752/0/27k/3432 -> 0/0/2300/0/21k/2680 | 3192/1568/2576/0/33k/384 -> 1152/576/1088/0/25k/384 | 0/0/2728/0/27k/3432 -> 0/0/1532/0/21k/2680 |
| replace | 0/109k/38k/0/288k/0 -> 0/77k/25k/0/316k/0 | same as A | same as A |
| raid5_second_fault | 0/67k/26k/0/164k/0 -> 0/51k/19k/0/183k/0 | same as A | same as A |
| nospare_persistent_full | 0/0/768/2432/13k/0 -> 0/0/384/1536/10k/0 | 0/0/768/0/13k/0 -> 0/0/384/0/10k/0 | same as CUR |
| cap2_missing_return | 0/0/0/0/8028/0 -> 0/0/0/0/12k/0 | 1788/576/1212/0/0/0 -> 1578/516/1062/0/5748/0 | 0/0/0/0/288/0 -> 0/0/0/0/3432/0 |
| raid6_second | 0/52k/117k/0/133k/0 -> 0/52k/107k/0/375k/0 | same as A | same as A |
| raid6_bad_missing_full | 0/5096/39k/0/274k/0 -> 0/2416/23k/0/291k/0 | 96k/33k/130k/32/146k/0 T6 -> 87k/31k/110k/0/215k/0 T6 | 0/8488/84k/0/377k/0 -> 0/4736/62k/0/402k/0 |

| family (csum) | A | CUR | A2 |
|---|---|---|---|
| transient | 0/0/0/0/33k/0 -> 0/0/0/0/27k/0 | same as A | same as A |
| persistent | 0/0/0/0/45k/0 -> 0/0/0/0/39k/0 | same as A | same as A |
| flush | 0/0/0/0/0/0 (=) | 0/0/0/0/0/0 (=) | 0/0/0/0/0/0 (=) |
| flush_unnamed | 0/0/0/0/4472/4472 (=) | 0/3696/0/0/2552/0 (=) | 0/0/0/0/4472/4472 (=) |
| crash_loss | 0/13k/0/0/13k/0 -> 0/12k/0/0/44k/0 | same as A | same as A |
| missing_return | 0/2544/0/0/3112/0 -> 0/1728/0/0/11k/0 | same as A | same as A |
| missing_return_full | 0/1312/0/0/19k/0 -> 0/896/0/0/29k/0 | 0/12k/0/0/23k/0 -> 0/11k/0/0/39k/0 | 0/8960/0/0/28k/0 -> 0/8544/0/0/35k/0 |
| missing_return_twice | 0/20k/0/0/20k/0 -> 0/14k/0/0/47k/0 | same as A | same as A |
| persistent_full | 0/0/0/0/11k/0 -> 0/0/0/0/9464/0 | same as A | same as A |
| replay_full | 0/0/1024/0/13k/1920 -> 0/0/768/0/11k/1536 | 0/0/0/0/16k/384 -> 0/0/0/0/13k/384 | 0/0/0/0/13k/1920 -> 0/0/0/0/11k/1536 |
| replay_full_transient | 0/0/2036/0/23k/3432 -> 0/0/1532/0/19k/2680 | 0/0/0/0/28k/384 -> 0/0/0/0/23k/384 | 0/0/1012/0/23k/3432 -> 0/0/764/0/19k/2680 |
| replace | 0/101k/0/0/243k/0 -> 0/76k/0/0/289k/0 | same as A | same as A |
| raid5_second_fault | 0/64k/0/0/140k/0 -> 0/50k/0/0/167k/0 | same as A | same as A |
| nospare_persistent_full | 0/0/0/2688/11k/0 -> 0/0/0/1664/9464/0 | 0/0/0/0/11k/0 -> 0/0/0/0/9464/0 | same as CUR |
| cap2_missing_return | 0/0/0/0/8028/0 -> 0/0/0/0/12k/0 | 0/0/0/0/0/0 -> 0/0/0/0/5748/0 | 0/0/0/0/0/0 -> 0/0/0/0/3156/0 |
| raid6_second | 0/77k/0/0/66k/0 -> 0/57k/0/0/212k/0 | same as A | same as A |
| raid6_bad_missing_full | 0/5384/0/0/259k/0 -> 0/3024/0/0/281k/0 | 0/22k/0/0/75k/0 T6 -> 0/17k/0/0/119k/0 T6 | 0/9176/0/0/302k/0 -> 0/5792/0/0/325k/0 |
| meta_persistent_full | 0/0/0/0/11k/11k -> 0/0/0/0/9276/9276 | same as A | same as A |

| family (nodatasum) | C | CTM |
|---|---|---|
| transient | 0/18k/149k/0/190k/0 -> 0/15k/75k/0/210k/0 | same as C |
| persistent | 0/19k/129k/0/180k/0 -> 0/16k/63k/0/197k/0 | same as C |
| flush | 0/19k/59k/0/84k/0 -> 0/16k/52k/0/152k/0 | same as C |
| crash_loss | 0/14k/27k/0/47k/0 -> 0/9272/20k/0/90k/0 | 0/14k/27k/0/47k/0 -> 0/13k/23k/0/83k/0 |
| missing_return_full | 24k/21k/19k/0/72k/0 -> 19k/16k/17k/0/86k/0 | 0/3312/9936/0/40k/0 -> 0/3312/9936/0/53k/0 |
| replay_full | 0/760/4076/0/16k/384 -> 0/760/3312/0/18k/384 | same as C |
| raid5_second_fault | 0/94k/39k/0/174k/0 -> 0/79k/33k/0/200k/0 | 0/93k/37k/0/163k/0 -> 0/80k/33k/0/191k/0 |
| nospare_persistent_full | 0/768/4736/0/18k/0 -> 0/768/3584/0/19k/0 | same as C |
| meta_persistent_full (csum) | 0/2032/0/0/5108/5108 (=) | (=) |

**Variants (nodatasum), original -> corrected:**

| Variant | Original | Corrected |
|---|---|---|
| persistent A+NI | 0/0/0/0/45k/0 | 0/0/0/0/39k/0 |
| persistent A+SD+NI = A2+SD+NI | 0/0/0/0/45k/0 | 0/0/0/0/39k/0 |
| persistent C+NI | 0/19k/33k/0/91k/0 | 0/17k/28k/0/122k/0 |
| persistent C+SD+NI | 0/0/0/0/110k/0 | 0/0/0/0/104k/0 |
| missing_return_full A+SD+NI | 0/192/192/0/22k/0 | 0/192/192/0/35k/0 |
| missing_return_full C+SD+NI (silent) | 11k/6096/5632/0/29k/0 | 11k/5884/5436/0/44k/0 |
| missing_return_full CTM+SD+NI | 0/384/640/0/32k/0 | 0/384/640/0/42k/0 |
| crash_loss A+SD+NI | 0/4288/4480/0/78k/0 | 0/3904/3712/0/106k/0 |

**Refused share, REF_WRITE / (REF_WRITE + WRITE_ACK), original -> corrected:**

| Cell | Original | Corrected |
|---|---|---|
| persistent A | 15.1% | 16.2% |
| persistent A+NI | 10.1% | 10.9% |
| persistent C | 16.4% | 23.5% |
| persistent C+SD+NI | 15.3% | 16.1% |
| transient A | 12.2% | 12.4% |
| transient C | 14.0% | 19.9% |
| missing_return_full A | 49.4% | 56.1% |
| missing_return_full C | 22.3% | 23.7% |
| missing_return_full CTM | 23.7% | 27.2% |
| nospare_persistent_full A | 28.8% | 26.4% |
| nospare_persistent_full C | 18.0% | 21.0% |

## Counterexamples

### A (also CUR, A2, and B, C, CB, CTM before their trigger): REFUSED_WRITE (model under-counts) (fidelity: the original acknowledges a write the kernel refuses; the acknowledged states after it do not exist in the kernel)

Scenario: persistent: a write into a named column while its device still fails

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || write s0.d0=3. Original model: `ok (write to {d0} failed)`, acknowledged. Corrected (F1): `REFUSED(phaseA {d0} write-back failed)`.

Kernel: rmw_prepare_repair() raid56.c:4155-4173 keeps the target column's sectors in repair_bitmap; rmw_repair_first() 4675-4812 writes them back with FUA and refuses at 4743 (EV_REFUSED). The model excludes the target at policy_model.py:523 and 643.

### A (also CUR, A2): LOST_ACKED unreachable (model over-counts A-1) (fidelity: A-1 inflated 2-3x (transient 54,728 -> 19,362; persistent 42,356 -> 14,102 states))

Scenario: transient: the device heals, then a crash in phase B of a write into the named column

Trace: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || d0 heals || write s0.d0=3 ; phaseB landed {} ; CRASH ; mount. Original: rec=T[0], s0.d0 EIO for good, data_ok=False. Corrected (F1): phase A restored d0=2 and cleared the name durably; the record is torn-only, recovery rewrites P, and s0.d0 reads 2 (data_ok=True).

Kernel: rmw_repair_first() calls btrfs_wib_clear_stale(durable) and btrfs_wib_persist_now() before phase B (raid56.c:4760-4808). A crash in phase B then leaves no stale name, so SCRUB_WIB_TORN (scrub.c:3519-3523) does not fire. A-1 itself remains for a crash between btrfs_wib_mark() (raid56.c:4939) and rmw_repair_first() (4977).

### A (also CUR, A2, C): LOST_ACKED gone and REFUSED_WRITE (degraded write hole over-reachable) (fidelity: LOST(gone) -35..45% in the detach families (crash_loss 14,200 -> 9,272; missing_return 2,544 -> 1,536))

Scenario: runtime detach, then a write into a stripe naming the removed device

Trace: d0 detaches || write s0.d0=2 ok || write s0.d1=2. Original: `ok`, so a crash in its phase B opens the degraded write hole. Corrected (F2): `REFUSED(phaseA {d0} write-back failed)`. The hole stays reachable through `unmount ; mount` (d0 now truly absent) || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount, which gives s0.d0 EIO with lost_info.

Kernel: btrfs_remove_bdev() super.c:2498-2540 sets MISSING and missing_devices++ (so CUR may evict) but keeps dev->bdev. rmw_prepare_repair() tests bdev only (raid56.c:4143); btrfs_submit_dev_bio() fails the write for MISSING (bio.c:714-720); rmw_repair_first() refuses (4743). Presence is judged by bdev in rbio_missing_cols() 4088-4096 and scrub.c:3441-3446.

### A, CUR (RAID6): STUCK / LOST unreachable (model over-counts) (fidelity: RAID6 records held that the kernel retires; minor)

Scenario: RAID6: a torn stripe naming one data column, both parities usable

Trace: (raid 6, transient) d0 starts failing writes || write s0.d0=2 ok || d0 heals || repair s0 ; CRASH after mark ; mount. Original: recovery keeps rec=T[0] forever. Corrected (F3): the column is rebuilt with the Q cross-check and the record retires. CUR's 32 STUCK states in raid6_bad_missing_full vanish under F3 alone.

Kernel: scrub_raid56_plan_wib() returns SCRUB_WIB_TORN only if hweight(holes) == nr_good_par (scrub.c:3504-3523), else PROVEN. The model's torn_blocks() ignores npars (policy_model.py:333-346, used at 880 and 968).

### CUR: eviction order (verdict evicted too early) (fidelity: CUR eviction order; reads and writes refuse either way)

Scenario: RAID5: SCRUB_WIB_TORN at mount on a present named column

Trace: d0 starts failing writes || write s0.d0=2 ok || repair s0 ; CRASH after mark ; mount. Original: rec=T[0], a plain naming record that CUR spends in pass 1. Corrected (F3): rec=TV[0,2], a verdict spent last (pass 3).

Kernel: btrfs_scrub_raid56_full_stripe() raid56_torn_undecided -> scrub_raid56_mark_suspect(present_par) (scrub.c:5713-5723, 5122); wib_entry_verdict() raid56-wib.c:503; pass 3 at 847-862.

### A as the kernel with keep_naming_degraded=1 and torn_unevictable=1 (what RESULTS.md calls A): SILENT_WRONG (silent wrong read under the 'strict' kernel configuration; the model's A avoids it only by assuming a kernel change)

Scenario: flush_unnamed: a failed flush the log is too full to name

Trace: d0 starts failing flushes || write s0.d0=2 (flush lost {d0}, log cannot name it: torn-only + alert) ok => s0.d0 reads 1. Control K_a_ack_unnamed: 24,508 silent states, identical on the corrected model. The WIB_READD_LOSE plan (records dropped outright) is not modelled and is at least as bad.

Kernel: wib_readd_dropped(): plan UNNAMED raid56-wib.c:3014, EV_LOG_UNFLUSHED 3196, message 3211, commit continues; plan LOSE 3016, nr_lost 3049. No knob makes either refuse.

### CUR: SILENT_WRONG (unmodelled path) (silent wrong read (CUR only; A and A2 never drop torn records))

Scenario: tree-log replay with a device missing (not in the matrix: no family combines --replay and --detach)

Trace: (kernel reasoning, not run) crash with a write in flight into a stripe whose data column is on a device missing at the next mount || mount with a tree log pending: recover_absent runs with classify=false, and the record is kept as plain @torn (kept_torn), not a verdict || the replay's RMW (LOG_RECOVERING) finds the log full, and pass 1 spends that record || btrfs_wib_recover_after_replay() never sees it || a degraded read rebuilds the absent nodatasum column from the torn parity and returns it without error

Kernel: scrub.c:5640-5659 (classify = !log_replay_pending); raid56-wib.c:7591 wib_keep_torn; wib_may_evict_naming() 725; pass 1 847-862. The model classifies before the replay (policy_model.py:848).

### A (and CUR with every device present): possible SILENT_WRONG (unmodelled; reported by the attack_A agent) (potentially silent. I verified the kernel lines, not the attack model's mapping.)

Scenario: log capacity halving when the first stale bit appears, with nothing evictable

Trace: (kernel reasoning) the live set exceeds 82 wide entries and nothing is evictable || btrfs_wib_commit() cannot build the block and 'keeps the previous one', returning 0 || the transaction commit acknowledges writes whose stale names are only in memory || crash. See attack_A/probe_A_kern.txt.

Kernel: wib_live_max() raid56-wib.c:527-537; wib_enforce_capacity_locked() 941-950; btrfs_wib_commit() 5757-5765 (warns 'block full, keeping the previous one', returns 0).

## Sensitivity

**Equivalence checks:**
- With `--fid none` the corrected copy reproduces the original model's RESULT lines exactly (persistent_full, A and CUR).
- Adding the WRITE_ACK count changed no other number: six cells rerun only to add it match their earlier corrected runs exactly (listed in `jobs_ack.txt`; the earlier outputs are in `out_superseded/*.noack.txt`).
- **Two fixes came after the corrected batch started:**
  - the target column counts for the budget only when it is named or absent;
  - the replay proxy's cell is exempt from checksum verification of its old value.
  
  Both touch only checksummed cells. Four nodatasum cells rerun on the final code are identical line for line (`out_check/`). Every checksummed cell that could have run on older code was rerun; the old outputs are in `out_superseded/`.

**Every A, CUR and A2 negative control still fires on the corrected model** (`out/K_*.txt`):

| Control | Original | Corrected |
|---|---|---|
| K_cur_degraded_evict (SILENT) | 38,464 | 34,124 |
| K_cur_replay_evict (SILENT) | 1,520 | 512 |
| K_cur_unnamed_flush (SILENT) | 24,508 | 24,508 |
| K_a_evict_naming (SILENT) | 39,964 | 38,632 |
| K_a2_no_taint (SILENT) | 24,320 | 19,380 |
| K_a_no_suspect (SILENT) | 34,798 | 29,606 |
| K_a_absent_par_unnamed (SILENT) | 27,920 | 36,486 |
| K_a_ack_unnamed (SILENT) | 24,508 | 24,508 |
| K_a_missing_wedge (REF_WRITE) | 28,024 | 36,276 |
| K_a_replay_down (RO) | 1,920 | 1,536 |
| K_a_nospare_stuck (STUCK) | 2,432 | 1,536 |

`K_a_read_trust_torn` stays at 0, as in the original; RESULTS.md §6 explains why. F1 makes that explanation hold for the target column too.

**Each fix changes the outcome it targets** (`demo_traces.txt`, produced by `walk.py`, run with `--fid none` and again with the fixes):
- F1: refusal while the device fails; recovery after a heal.
- F2: refusal on a runtime-removed device, while the degraded-mount hole is kept.
- F3: the RAID6 stripe is retired, and the RAID5 verdict is recorded as TV[0,2].

**Attribution** (each fix alone, `out/X_*.txt`):
- F1 accounts for all of the A-1 reduction in the failing-disk and replay families.
- F2 accounts for most of the LOST(gone) reduction in the detach families.
- F3 alone removes CUR's 32 STUCK states in raid6_bad_missing_full.
- The CUR silent classes and their shortest traces are unchanged after correction (`out/M_*_CUR_nodatasum.txt`). A and A2 have 0 silent states in every cell.

## Caveats

**1. `REPORT.md` was not written.** The Write call was refused by the harness ("Subagents should return findings as text, not write report files"), and I did not work around it. This output is the report. Supporting data, scripts and runs are in `fidelity/`.

**2. What the corrected model still leaves out:**
- D5: layout halving, 4 MiB regions, and the transaction commit's "keep the previous block" at raid56-wib.c:5757-5765.
- D4: the readd LOSE plan.
- D6: classification deferred at replay with a device missing.
- D7 and D8: torn handling in the RMW path and after a successful RMW.

Each is argued from code with file:line, not run. D5 is being attacked by the attack_A agent (`attack_A/probe_A_kern.txt`, which reports silent states for A); I checked the kernel lines it relies on but not its model.

**3. Proxy semantics remain.** One sector per column, and in-place cells although the kernel forces COW on RAID5/6 (inode.c:1880-1920). The replay writes a data cell, although the kernel's replay writes only metadata, so CUR-3 and A-4 need RAID5/6 metadata (D12). F1 had to exempt the replay proxy's checksummed cell from verifying its old value. Without that exemption the proxy produced spurious mount failures that I traced and removed.

**4. REF_WRITE counts are raw transitions over different state spaces.** Compare the refused shares instead; the counts are for ranking within one policy only.

**5. Runs.** 204 corrected runs at the original depths, nice 10, at most 3 in parallel, none over about 5 minutes. raid6_bad_missing_full for CUR still stops at T6 (memory), as in the original.

**6. No kernel runs.** None of the divergences was reproduced on a UML kernel.
- F1: a dm-flakey `error_writes` disk named stale, then a write into the same column, should get EIO (raid56.c:4743), where the original model acknowledges it.
- F2: hot-removing a device (`echo 1 > /sys/block/X/device/delete`) and then writing into a stripe that names it should be refused.

**7. The kernel branch had not moved.** It was still `2a1e2379e4` throughout, and I modified nothing under `/home/user`.

## Model files

- <scratch>/fidelity/policy_model_fid.py
- <scratch>/fidelity/policy_model_fid.md5
- <scratch>/fidelity/policy_model.orig.py
- <scratch>/fidelity/walk.py
- <scratch>/fidelity/demo_traces.txt
- <scratch>/fidelity/run_fid.sh
- <scratch>/fidelity/tables.py
- <scratch>/fidelity/compare.py
- <scratch>/fidelity/jobs_main.txt
- <scratch>/fidelity/jobs_var.txt
- <scratch>/fidelity/jobs_attr.txt
- <scratch>/fidelity/jobs_c.txt
- <scratch>/fidelity/jobs_ctl.txt
- <scratch>/fidelity/jobs_orig.txt
- <scratch>/fidelity/out/
- <scratch>/fidelity/out_superseded/
- <scratch>/fidelity/out_check/
