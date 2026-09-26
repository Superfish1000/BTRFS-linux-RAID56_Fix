# break_A

## Attack on policy A (strict refusal): result

**Bottom line**
- **On the model's own rules, A holds.** 22 deeper and wider runs found no silent wrong read and no new loss class.
- **The kernel cannot get A just by setting its two knobs** (`raid56_keep_naming_degraded=1`, `raid56_wf_torn_unevictable=1`). I found five kernel paths where A's promise is not enforced, three of them silent with no raid56 alert.
- **A done strictly on this log is never silent, but it can wedge for good.** A small change to the log's capacity (R1 below) removes the most important hole and the wedge together.

**Deliverable.** REPORT.md was not written: the harness refuses report files from subagents, so this output is the report. Everything else is in `attack_A/`: the model is `attack_model.py`, and there is one output file per run in `out/` (187 runs).

### How I attacked it
1. **Same rules, pushed harder** (22 `D_*` runs). Depth 11; 3 full stripes; two simultaneous faults on RAID5 and RAID6; metadata; mixed data; tree-log replay with a missing device; admin closure of 5 actions; values that cannot alias.
   - Silent wrong reads: **0**.
   - Losses: only the known loud ones (A-1 torn and undecidable, A-2 degraded write hole, and double faults).
   - STUCK: only the known no-spare wedge, which still holds with an admin closure of 5 (5,116 and 5,372 states).
   - `D_fresh_persistent` reproduces the shared model's A numbers exactly.
2. **Kernel mechanisms the shared model leaves out,** added one per switch to a copy of the model:
   - the two log layouts: 165 regions while nothing names a member, 82 once something does;
   - the on-disk block tracked apart from the in-memory set;
   - mount recovery bounded by the table size, in any order;
   - a concurrency window;
   - a scrub that sees only the commit root;
   - unlogged full-stripe writes;
   - a device returning while mounted.

   With every new switch off the copy matches `policy_model.py` exactly (6 of 6 runs).
3. **Reading the kernel code** (`raid56-wib.c`, `raid56.c`, `scrub.c`, `disk-io.c`, `bio.c`) at stage0-wip 2a1e2379e4.

### Findings
- **K3 (most important): the capacity halves, and the failure to write the log is ignored.**
  - The first record that names a member switches the log to the wide layout: 165 regions become 82 (`wib_live_max`). Under A nothing can be evicted, so the set can exceed what any block describes.
  - `btrfs_wib_commit()` logs "block full, keeping the previous one" and returns 0, so the transaction commits and the write is acknowledged. The name exists only in memory.
  - At the next mount the stripe reads as a write in flight, and the recovery rebuilds its parity from the stale column: the acknowledged data is gone. **No raid56 alert**; the only signs are log lines, and later writes failing as "log full". With `CONFIG_BTRFS_ASSERT` it is a BUG instead (the ASSERTs at raid56-wib.c:3437 and 5397).
  - The same happens through a device replace's zero-marks (K3r): the zeros read back as data.
  - Model, nodatasum: `A_kern` is silent in 8 of 8 capacity families, from 208 to 35,450 states. `A_strict` has 0, and fix R1 has 0.
  - Kernel trigger: a crash while the log lists more than 82 regions, a mount with a device missing, then any write that touches that device. Also: more than 82 torn-only records (from a failed flush the log could not name, or from parity writes the recovery could not make), then any failed write.
- **K1 (race).** Between `btrfs_wib_done(failed)` (raid56.c:5076) and `rmw_update_stale_data()` (5128), a failed write's record names nothing. Eviction pass 0 (raid56-wib.c:854) may spend it, which A allows, and the name then finds no entry. The stale column is read directly; `EV_DROPPED` was raised. Model: 34,738 and 43,430 silent states, the same for A, CUR and A2.
- **K4 (scrub).** A user scrub looks extents up in the commit root. It retires the whole stripe's record (scrub.c:3843 or 4004) while a failed write of the running transaction is still in it, after regenerating the parity from the stale column. Silent, no alert, and the loss is permanent. Model: 11,224 states (25,584 with flush faults). Unlike device replace, scrub does not commit before it starts.
- **K5 (full-stripe writes).** Full-stripe copy-on-write writes are not logged. A disk that takes one into its cache and then fails the commit's flush leaves a stale column nothing names, because the written-member tracking keeps nothing for regions the log does not list. Silent, no raid56 alert. Model: 11,080 states; 0 when the loss is named.
- **K2 (mount recovery drops).** `btrfs_wib_add_sticky()` drops a kept record that finds no room (`EV_DROPPED`) whatever the policy, and the mount goes on read-write. For an error record the drop happens before its scrub, which then trusts nodatasum data.
  - The kernel already avoids the common case: `btrfs_wib_parity_unwritten()` keeps a missing-parity stripe torn when room is short.
  - What remains: the pending set is the union of every device's newest block, which can exceed one block. Model: 8,888 silent states with a union of blocks.
- **K6 (by design).** The failed-flush re-add plans that lose records (the LOSE plan) or leave them unnamed (UNNAMED, which is CUR-4) are drops that A forbids.

### A done strictly (`A_strict`)
- **Never silent in any run.** "Strictly" means: the commit aborts to read-only, a replace whose marks cannot be written fails, and a mount that cannot keep every record stays read-only.
- **It wedges.** Take a crash with more regions listed than the wide layout holds, then a disk gone for good or failing writes at mount. The recovery's records can never be written, so every mount stays read-only, and a replace needs a writable mount. STUCK: 56, 704 and 7,400 states in three families, 824 with a union of blocks.
- **A2 done strictly has no such wedge (0 STUCK)**, and neither does A under R1.

### Recommended fixes (model-checked where marked)
- **R1 (model-checked).** Never admit more records than the wide layout holds: cap the live set at 82 even while blocks are still written narrow. The on-disk format does not change; the cost is a log half the size.
  - Under R1, `A_kern` and `A_strict` give identical results in 5 of 5 families: 0 silent, 0 read-only, 0 STUCK. The only exception is a RAID6 STUCK that is a model artifact.
  - R1 does not help when the mount-time union of blocks exceeds one block; that case needs R6.
- **R2.** `btrfs_wib_commit()`, the replace marks and `add_sticky` at mount must refuse — abort the commit, fail the replace, keep the mount read-only — rather than go on or drop.
- **R3 (K1).** Name the member under the same lock that clears the in-flight bit.
- **R4 (K4).** Scrub must keep records made after the commit root (compare the entry's generation), or commit after marking the RAID56 block group read-only, as replace does.
- **R5 (K5).** Track unlogged full-stripe writes as written and name them after a failed flush, or abort the commit.
- **R6 (K2, K6).** The recovery and the re-add must stay read-only rather than drop, or use A2's drop-and-taint.

## Table

Each cell is SILENT (of which with no alert) / LOST(gone) / LOST(unreach) / STUCK / REF_WRITE / RO. SILENT, LOST and STUCK count distinct states. REF_WRITE and RO count distinct transitions. k means thousands.

- **Tn:** the run stopped on its time or memory limit at depth n.
- **Row labels:** the depth in brackets is the depth of the complete runs.
- **Capacity families:** RAID5 unless noted, 2 full stripes, capacity 2 records narrow and 1 wide. `cap3` has 3 stripes, 3 narrow and 2 wide.

**Variants:**
- **A_strict:** A as defined; a set that cannot be written refuses.
- **A_kern:** A's eviction rule on the stage-0 commit path, with the unnamed-flush abort fixed.
- **AK:** A_kern plus acknowledging a flush loss the log cannot name. This is the kernel with A's two knobs, exactly.
- **A2_* and CUR_kern:** for comparison.

**Capacity families, nodatasum**

| family | A_strict | A_kern | AK | A2_strict | A2_kern | CUR_kern |
|---|---|---|---|---|---|---|
| cap_degraded (d8) | 0/1312/2904/0/40k/11k | 2338(2338)/4834/8702/0/67k/0 | 2338(2338)/4834/8702/0/67k/0 | 0/9392/38k/0/78k/0 | 2268(916)/12k/40k/0/86k/0 | 25k(0)/23k/25k/0/49k/0 |
| cap_degraded_gone (d8) | 0/1816/6864/56/35k/11k | 976(976)/3216/8574/0/61k/0 | 976(976)/3216/8574/0/61k/0 | 0/5440/17k/0/37k/0 | 2268(916)/8324/18k/0/45k/0 | 3740(0)/9180/17k/0/30k/0 |
| cap_two_losses (d8) | 0/7224/5392/0/105k/23k | 15k(15k)/24k/15k/0/151k/0 | 15k(15k)/24k/15k/0/151k/0 | 0/80k/125k/0/326k/0 | 0/80k/125k/0/326k/0 | 267k(0)/268k/49k/0/122k/0 |
| cap_flush_unnamed (d8) | 0/4380/22k/1100/225k/49k | 208(208)/4588/22k/1100/245k/49k | 117k(40)/77k/59k/0/142k/0 T6 | 0/4380/22k/1100/225k/49k | 208(208)/4588/22k/1100/245k/49k | 123k(0)/79k/65k/0/143k/0 T6 |
| cap_raid6 (d7) | 0/6944/52k/4294/391k/74k | 18k(15k)/23k/86k/2934/560k/0 | 18k(15k)/23k/86k/2934/560k/0 | 0/7276/112k/1684/343k/72k T6 | 11k(4904)/15k/91k/1320/230k/0 T5 | 92k(0)/31k/88k/1158/84k/0 T5 |
| cap_replay (d7) | 0/5656/14k/704/170k/42k | 7288(4512)/12k/24k/0/227k/31k | 7288(4512)/12k/24k/0/227k/31k | 0/24k/93k/0/331k/59k | 272(0)/24k/93k/0/344k/42k | 56k(0)/44k/48k/0/300k/24k |
| cap_ret_runtime (d8) | 0/22k/22k/7400/324k/44k | 35k(28k)/56k/53k/0/561k/0 | 35k(28k)/56k/53k/0/561k/0 | 0/79k/197k/0/747k/67k | 18k(7218)/83k/148k/0/596k/0 T7 | 235k(1016)/183k/146k/0/619k/0 T7 |
| cap3_degraded (d7) | 0/14k/34k/0/282k/49k | 6770(6770)/28k/65k/0/475k/0 | 6770(6770)/28k/65k/0/475k/0 | 0/55k/234k/0/292k/0 | 3626(3626)/59k/235k/0/305k/0 | 68k(0)/91k/182k/0/213k/0 |

**Capacity families, checksummed data.** SILENT is 0 in every cell. LOST(gone) and STUCK:

| family | A_strict | A_kern / AK | A2_strict | CUR_kern |
|---|---|---|---|---|
| cap_degraded | 1472, 0 | 1568, 0 | 8288, 0 | 11k, 0 |
| cap_degraded_gone | 1592, 0 | 1632, 0 | 3784, 0 | 5824, 0 |
| cap_two_losses | 4216, 0 | 4216, 0 | 32k, 0 | 39k, 0 |
| cap_flush_unnamed | 4144, 54 | 4144 / 49k T7, 54 / 61 | 4144, 54 | 55k, 88 T7 |
| cap_raid6 | 6752, 176 | 7624, 0 | 12k, 0 | 16k, 0 T6 |
| cap_replay | 3952, 64 | 3952, 0 | 16k, 0 | 18k, 0 |
| cap_ret_runtime | 19k, 1606 | 25k, 132 | 70k, 142 | 91k, 200 |
| cap3_degraded | 8916, 0 | 8916, 0 | 27k, 0 | 27k, 0 |

**Fix R1: live set capped at the wide capacity (1/1), nodatasum**

| family | A_strict | A_kern | A2_strict |
|---|---|---|---|
| cap_degraded | 0/1312/2904/0/27k/0 | 0/1312/2904/0/27k/0 | 0/6720/20k/0/72k/0 |
| cap_degraded_gone | 0/864/2200/0/21k/0 | 0/864/2200/0/21k/0 | 0/3784/8792/0/32k/0 |
| cap_two_losses | 0/7224/5392/0/70k/0 | 0/7224/5392/0/70k/0 | 0/48k/65k/0/251k/0 |
| cap_ret_runtime | 0/22k/22k/0/267k/0 | 0/22k/22k/0/267k/0 | 0/53k/103k/0/568k/0 |
| cap_raid6 (STUCK is the RAID6 cross-check artifact) | 0/6880/51k/2046/326k/0 | 0/6880/51k/2046/326k/0 | 0/10k/120k/1974/454k/0 T6 |

**Union of divergent device blocks** (`--union-extra`, depth 7, nodatasum), with MOUNTDROP transitions

| run | cell | MOUNTDROP |
|---|---|---|
| A_strict | 0/5968/7820/824/135k/24k | 0 |
| A_kern | 7288(4512)/15k/23k/0/242k/0 | 14984 |
| A, runtime strict with the kernel's mount-time drops | 0/11k/14k/0/190k/10k | 16496 |
| A, R1, A_kern | 8888(0)/10k/12k/0/212k/0 | 44368 |
| RAID6, runtime strict with mount-time drops | 0/4056/42k/1456/408k/53k | 57624 |
| A2_strict | 0/28k/87k/0/280k/56k | 0 |

**Implementation windows, nodatasum** (the same for A, CUR and A2 unless shown)

| family | A | CUR | A2 |
|---|---|---|---|
| race_vague (K1, d9) | 35k(0)/21k/9772/0/69k/0 | same | same |
| race_vague_transient (d9) | 43k(0)/25k/16k/0/111k/0 | same | same |
| scrub_uncommitted (K4, d9) | 11k(11k)/11k/6216/0/22k/0 | same | same |
| scrub_uncommitted_flush (d9) | 26k(26k)/26k/22k/0/20k/0 | same | same |
| ret_runtime (d9) | 0/2320/5968/0/9376/0 | same | same |
| ret_runtime_full (d9) | 0/960/2816/0/30k/0 | 39k(0)/25k/25k/0/87k/0 | 0/4832/17k/0/97k/0 |
| fullstripe_flush (K5, d8), kern / strict (named) | 11k(11k)/5640/23k/0/11k/0 / 0/0/20k/0/11k/0 | same | same |

With checksummed data the windows give SILENT 0 and loud losses instead: K1 1504 (CUR 3056), K4 9392 (17k with flush), K5 800 (kern) and 0 (strict).

**Deeper push, policy A, shared semantics**

| run | states | depth | cell |
|---|---|---|---|
| persistent_d11 nodatasum / csum | 159k / 128k | 11 | 0/0/45k/0/97k/0 ; 0/0/0/0/52k/0 |
| transient_d11 nodatasum / csum | 204k / 168k | 11 | 0/0/58k/0/98k/0 ; 0/0/0/0/36k/0 |
| s3_persistent_cap2 nodatasum / csum | 126k / 114k | 8 | 0/0/18k/0/131k/0 ; 0/0/0/0/115k/0 |
| raid5_twofault nodatasum / csum | 355k T7 / 653k | 7-8 | 0/0/188k/0/239k/0 ; 0/0/0/0/361k/0 |
| raid6_twofault nodatasum / csum | 427k T6 / 398k | 6-7 | 0/0/53k/0/45k/0 ; 0/0/0/0/38k/0 |
| replay_missing_ret nodatasum / csum | 74k / 73k | 8 | 0/6272/16k/0/185k/38k ; 0/4304/9600/0/174k/38k |
| meta_detach (metadata) | 145k | 8 | 0/50k/0/108/178k/178k |
| mixed_persistent | 95k | 10 | 0/0/15k/0/47k/0 |
| missing_twice_d10 nodatasum / csum | 261k / 184k | 10 | 0/84k/38k/0/135k/0 ; 0/40k/0/0/45k/0 |
| nospare_admin5 nodatasum / csum | 6k / 6k | 9 | 0/0/768/5116/13k/0 ; 0/0/0/5372/11k/0 |
| fresh_persistent nodatasum / csum | 266k / 234k | 10 | 0/0/42k/0/81k/0 ; 0/0/0/0/45k/0 |
| fresh_missing_full nodatasum / csum | 19k / 16k | 10 | 0/1312/2912/0/35k/0 ; 0/1312/0/0/27k/0 |

## Counterexamples

### A as the kernel implements it (A_kern / AK: keep_naming_degraded=1, torn_unevictable=1): SILENT_WRONG with no record_dropped or log_unflushed alert, then LOST_ACKED (gone) (Silent wrong read, and the value is then gone from the disks. Only rate-limited log lines, and later writes failing as log full; no raid56 alert about the loss. A_strict: 0. Fix R1: 0.)

Scenario: K3: the log capacity halves when the first record names a member; degraded mount after a crash (cap3_degraded, cap_degraded, cap_two_losses; 976 to 14,860 silent states)

Trace: write s0.d0=2 ; CRASH after mark (log also lists s1 in flight) ; mount without d0 [s0 and s1 kept as recovery verdicts TV[P]: narrow, 2 of 3] || write s2.d0=2 [names the missing d0: the set now needs the wide layout and holds 3 > 2 records; 'log block full: commit keeps the previous block, goes on'] ok || unmount ; mount with {d0} back [the devices hold the last block that could be written, which lists s2 in flight] => the recovery recomputes s2's P from d0's stale platter, and s2.d0 reads 1 (acknowledged 2) with no error

Kernel: Applies (code reading; not run on UML).
- btrfs_wib_mark_stale() (raid56-wib.c:3797) calls wib_enforce_capacity_locked() (941, at 3831). The first stale bit makes wib_live_max() (529) return 82 instead of 165.
- With keep_naming_degraded=1 (592/722) and torn_unevictable=1 (621), wib_evict_sticky() (801) finds nothing to evict, and the set stays over the limit (the comment at 936-939 says so).
- write_all_supers() calls btrfs_wib_commit() (disk-io.c:4244). On -ENOSPC it warns 'block full, keeping the previous one' and returns 0 (raid56-wib.c:5758-5776), so the transaction commits and acknowledges the write.
- btrfs_wib_commit_prepare() has ASSERT(ret == 0) (5397) and wib_flush_and_drop_locked() has another (3437). They fire only with CONFIG_BTRFS_ASSERT.
- btrfs_wib_unmount() (5528) fails with 'failed to write the log: -28'.
- At the next mount the region is listed in flight with no error, so the recovery runs in mode TRUSTED (7540-7542) and recomputes the parity from the stale column.
- Kernel trigger: a crash while the log lists more than 82 regions (all narrow: verdicts and torn marks stay narrow, and btrfs_wib_parity_unwritten() keeps missing-parity stripes torn when the pending set exceeds 82, 4572-4581), a degraded mount, then any write touching the missing device's column or parity.
- The same with every device present: more than 82 torn-only records (UNNAMED re-add, or parity writes the recovery could not make, wib_keep_torn() at 4516), then any failed write. Model: A_kern cap_flush_unnamed (208 states), cap_ret_runtime (35,450), cap_raid6 (18k).

### A_kern / AK (also A2 on the kernel's commit path): SILENT_WRONG with no alert (Silent wrong read (zeros served as file data). A2_kern: 916 to 7,218 no-alert states. A_strict and A2_strict: 0.)

Scenario: K3r: a device replace's zero-marks push the set over the wide capacity (cap_degraded, cap_degraded_gone)

Trace: write s0.d0=2 ; CRASH after mark (log also lists s1 in flight) ; mount without d0 [two verdicts, narrow] || replace d0 [writes zeros where it can neither copy nor rebuild and marks them stale: the set becomes wide, 2 > 1; the marks exist only in memory] || unmount ; mount (log read back: the last block written) => s0.d0 and s1.d0 read the replace's zeros as data

Kernel: Applies.
- btrfs_wib_replace_mark_stale() (raid56-wib.c:3910) calls wib_enforce_capacity_locked() at 3942. Under A nothing is evicted, so the entry survives and the function returns true (3943-3944), and the replace writes the zeros.
- The commits then swallow -ENOSPC as in K3, and btrfs_wib_replace_end() hands over marks that never reached the disk.
- CUR does not show this: with the device missing it evicts, and spending the replace's own marks makes the replace fail (pass 2).

### A_kern with the kernel's mount recovery, even under R1: SILENT_WRONG (record_dropped alert raised at mount) (Silent at the read, loud at mount. 8,888 states (R1, union). MOUNTDROP 14,984 to 57,624 transitions in the union runs. 0 silent once the recovery refuses (A_strict), at the cost of STUCK 824.)

Scenario: K2: mount recovery drops a record that finds no room; the pending set is the union of devices' newest blocks, which can exceed one block

Trace: write s0.d0=2 ; phaseB landed {d0} ; CRASH (an older block on one device also lists s1 in flight) ; mount without d1 (recovery order s1,s0) ; no room for s0's verdict TV[P]: dropped (EV_DROPPED) => s0.d1 reads a rebuild from the torn P (garbage), with no error on the read

Kernel: Applies, by design.
- btrfs_wib_add_sticky() (raid56-wib.c:3763-3782) drops a record that finds no room, with EV_DROPPED, whatever the policy.
- The recovery calls it before an error record's scrub (7548); the scrub then decides with no record and trusts the nodatasum data. It also calls it after an in-flight record is kept (7576).
- scrub_raid56_mark_suspect() (scrub.c:5130) uses it for verdicts, and itself warns that a read 'may return a wrong rebuild' (5147-5158).
- wib_recover_one() turns -EIO into 'kept' (7335-7342), so the mount continues read-write.
- The union comes from btrfs_wib_finalize_pending() (6818-6850): a device that failed log writes ('continuing', in wib_write_all_devices()) keeps an older, different block.
- The common missing-parity case is already guarded by btrfs_wib_parity_unwritten(). Once the model mirrors that guard, no drop happens without a union.

### A (and CUR, A2: identical): SILENT_WRONG (record_dropped raised); LOST(gone) after a later torn write (Silent wrong read, then permanent loss. With checksums it becomes a loud loss (1,504 states).)

Scenario: K1: race between btrfs_wib_done(failed) and rmw_update_stale_data() with the log full (race_vague: 34,738 states; transient: 43,430)

Trace: d0 starts failing writes || write s0.d0=2 RACE(a concurrent mark into s1 evicts the still-vague record; the names find no entry) ok (write to {d0} failed) => s0.d0 reads 1 (acknowledged 2) from the stale platter || write s0.d0=3 ; CRASH after mark ; mount => the recovery recomputes P from the stale d0: the value is gone

Kernel: Applies (a race; plausible, needs concurrent failing RMWs and a full log).
- rmw_rbio() calls btrfs_wib_done(..., failed) at raid56.c:5076. That clears the in-flight bit and sets only @sticky (raid56-wib.c:3691-3725).
- rmw_update_stale_data() and rmw_update_stale_parity() run afterwards (raid56.c:5128-5129), each retaking wib->lock.
- In between, the entry names nothing. wib_evict_sticky() pass 0 (raid56-wib.c:854) may spend it for a concurrent btrfs_wib_mark() or wib_enforce_capacity_locked(), and A allows that.
- btrfs_wib_mark_stale() then marks only where the stripe is still recorded (3819-3826), so the member is never named.
- The same window exists for full-stripe writes: btrfs_wib_try_add_sticky() then rmw_update_stale_data() (raid56.c:5154-5168).

### A (and CUR, A2: identical): SILENT_WRONG with no alert and LOST_ACKED (gone) (Silent and permanent, no alert (only a 'parity did not describe its data' warning line). With checksums, a loud loss (9,392 and 17,084 states).)

Scenario: K4: a user scrub between a write's RMW and the commit that acknowledges it; the scrub sees the commit root (scrub_uncommitted: 11,224 states; with flush faults: 25,584)

Trace: d0 starts failing writes || write s0.d0=2 ; SCRUB before the commit (commit root: s0.d0 is free space) regenerates P from the stale d0 and retires the record ; ok (write to {d0} failed) => s0.d0 reads 1, and the acknowledged 2 is on no disk

Kernel: Applies (a plausible window: the extent must be written in the transaction that is running when the scrub reaches its stripe).
- The scrub searches the commit root (scrub.c:587-590, 3773-3775).
- scrub_raid56_plan_wib() (3357) asks for help only for named columns with unverifiable extents in that commit root, so it returns NONE here.
- The parity is regenerated from cached on-disk data for rows that hold committed extents.
- btrfs_wib_clear_sticky() then retires the whole full stripe's record (scrub.c:4003-4004, or 3843 when no committed extent is seen), and it has no generation check (raid56-wib.c:4793).
- Device replace commits before copying (finish_extent_writes(), scrub.c:4361-4371, used at 4570-4575). A user scrub only marks the block group read-only.

### A (and CUR, A2: identical): SILENT_WRONG with no alert (Silent. There is no raid56 alert for that stripe, only the device's flush-error counter.)

Scenario: K5: an unlogged full-stripe COW write whose column a disk's failed flush loses (fullstripe_flush: 11,080 states; 0 when the loss is named)

Trace: delete the files in s0 || d0 starts failing flushes || fullwrite s0=[2,2] ok (flush lost {d0}; not listed, so nothing names it) => after the cache loss, s0.d0 reads 1 (acknowledged 2) || write s0.d0=3 ; CRASH after mark ; mount => P is recomputed from the stale d0: gone

Kernel: Applies.
- Full-stripe COW writes are not logged (raid56.c:4926-4940).
- btrfs_wib_note_written() keeps nothing for a region the log does not list (raid56-wib.c:2203-2262), so the re-add after a failed flush (wib_readd_dropped()) has nothing to name.
- barrier_all_devices() fails the commit only beyond the tolerance (btrfs_check_rw_degradable()), so the transaction referencing the new extent commits although that device may have lost the column from its cache.
- The comment at raid56.c:4926-4936 assumes those writes 'have completed and been flushed'.

### A_strict (A as defined, on the kernel's two log layouts): STUCK (Availability only: no loss and no silent read, but the filesystem can never be written again without dropping records.)

Scenario: A crash while more regions are listed than the wide layout holds, then a disk gone for good or failing writes at mount (cap_degraded_gone 56, cap_replay 704, cap_ret_runtime 7,400, union 824)

Trace: write s0.d0=2 ; CRASH after mark (log also lists s1 in flight) ; mount without d0 || P starts failing writes || unmount ; mount with {d0} back ; the recovery cannot keep s1's record: the mount stays READ-ONLY (every later mount does the same; replace needs a writable mount). Variant with d0 gone for good: every write naming d0 aborts its commit, and the replace's marks cannot be written.

Kernel: Not the kernel as it is: the kernel goes on and loses the name instead (K2, K3). This is the price of enforcing A on this log format. R1 removes it, except for a union of divergent blocks. A2_strict has 0 STUCK in the same runs, because it drops the missing disk's names with a taint.

### A (shared semantics): STUCK (Availability: 5,116 states (nodatasum) and 5,372 (csum). This confirms the matrix result is not an artifact of its 3-action admin closure.)

Scenario: Known A-3: a failing disk, a full log and no spare disk; re-checked at depth 9 with an admin closure of 5 actions

Trace: P starts failing writes || write s0.d0=2 ok (write to {P} failed) [log full of P's name] || every write into s1 REFUSED(log-full); with no spare disk no admin sequence of at most 5 actions restores writes

Kernel: Applies with keep_naming_degraded=1: btrfs_wib_mark() fails at WIB_ROOM_NONE (raid56-wib.c:3636).

## Sensitivity

**1. With every new switch off, `attack_model.py` reproduces `policy_model.py`.** Six runs gave identical RESULT lines (`regression_check.log`): A, CUR, A2 and C across persistent_full, missing_return_full, flush_unnamed, replay_full, cap2 and persistent.

**2. Every new failure class has an on/off pair in the same model, and turning the mechanism off removes it:**

| Class | On | Off |
|---|---|---|
| K3 | A_kern: silent in 8 of 8 nodatasum capacity families | A_strict (only the overcap rule differs): 0; fix R1 (wide cap = narrow cap): 0 in 5 of 5 families |
| K1 | race_vague: 34,738 / 43,430 silent states | the same families without the race (the shared persistent_full and transient A runs, and D_persistent_d11): 0 |
| K4 | scrub_uncommitted: 11,224 | the same family without the blind scrub: 0 (D_persistent_d11) |
| K5 | kern: 11,080 | the flush loss named (strict): 0 |
| K2 | union runs with the kernel's mount-time drop: MOUNTDROP 14,984 to 57,624 transitions | A_strict and A2_strict: MOUNTDROP 0 |

**3. Checksums turn every new silent class into a detected (loud) loss:** 0 silent in every checksummed cell, and LOST(gone) is where the damage shows.

**4. The model's A numbers are stable under the settings I added.** D_fresh_persistent (values that cannot alias, plus mismatch-only parity writes) reproduces the shared model's A persistent row exactly: 0/0/42,356/0/81,300. Mismatch-only parity writes alone leave SILENT and LOST unchanged on persistent_full and nospare.

**5. Three fidelity corrections each visibly changed results, which shows the model sees these details.** Superseded outputs are kept in `out_preguard/` and `out_prefix2/`.
- Missing-parity guard (`btrfs_wib_parity_unwritten`): without it the model had a "drop at mount" silent class through a missing P, which the kernel avoids.
- Failed parity write at recovery keeps the stripe torn: without it the model named P, which is wide.
- The parity scrub writes only rows that do not match: without it the scrub named a failing P on consistent stripes, and even R1 showed 4,230 silent states.

**6. The shared model's 22 negative controls still apply to the unchanged core.**

## Caveats

**The report file.** REPORT.md was not written: the harness refuses report files from subagents. This output is the report.

**What the model is, and is not:**
- Capacities of 2 narrow / 1 wide records (3/2 in `cap3`) stand in for the kernel's 165/82 regions. The model shows how each mechanism reaches the loss; the kernel trigger is argued from the code: more than 82 regions listed at a crash, which the lazy drop allows (up to 165 regions written since the last transaction commit).
- There is no parity rotation: P is always on the last device, and a column is one sector.
- The model runs one operation at a time. K1 and K4 are single injected interleavings, so the model cannot say how often those windows are hit. K5 uses an abstract full-stripe operation.
- K2 needs `--union-extra`, an abstraction of devices holding different newest blocks. Per-device log slots are not modelled.

**Nothing here was reproduced on a UML kernel.** Suggested arms:
- **K3:** `keep_naming_degraded=1`, `torn_unevictable=1`, `commit=300`, nodatasum. Make small writes to more than 82 distinct 4 MiB regions, crash with `raid56_crash_point`, mount degraded, write once to the missing device's column, unmount, re-add the device, mount, read. Expect "block full, keeping the previous one" in the log and the stale content returned.
- **K4:** scrub a RAID5 block group while failing writes are being made to it and before their transaction commits.
- **K5:** dm-flakey flush errors after a full-stripe write, then drop the device cache.

**Model artifacts, not findings:**
- The shared scrub and recovery refuse every torn stripe with a named column, without RAID6's spare-parity cross-check. This gives STUCK of 1,158 to 4,294 states in `cap_raid6` for every variant, including R1.
- The shared scrub treats a named checksummed column as a hole. This gives the STUCK 108 in `D_meta_detach`.
- The STUCK 1,100 in `cap_flush_unnamed` is a RAID5 double fault: one disk failing writes and another failing flushes, which can never be healed in the persistent family.

**Search bounds:**
- Depth 6 to 11; 1 or 2 crashes; 1 or 2 faults; 1 or 2 device losses; admin closure of 3 actions, and 5 for the nospare check.
- Runs that stopped on their time or memory limit (marked T): D_raid5_twofault and D_raid6_twofault with nodatasum; AK and CUR in cap_flush_unnamed; A2 and CUR in cap_raid6; A2_kern and CUR in cap_ret_runtime; A2_strict in R1 cap_raid6.
- Counts are distinct states and depend on how much each state space grows. Compare zero against non-zero, and the traces, rather than raw magnitudes.

**Which A the runs test.** "A_kern" assumes the unnamed-flush abort (CUR-4 fixed); "AK" is the kernel with A's two knobs exactly. K1, K4 and K5 are policy-independent and hit CUR and A2 the same way.

**Kernel lines** are at stage0-wip 2a1e2379e4, read without modifying anything under /home/user.

**Machine.** Shared, load 5 to 7; every run at nice 10, 2 to 4 in parallel, each under 25 minutes.

## Model files

- <scratch>/attack_A/attack_model.py
- <scratch>/attack_A/policy_model.orig.py
- <scratch>/attack_A/run_attack.sh
- <scratch>/attack_A/run_attack2.sh
- <scratch>/attack_A/run_prio.sh
- <scratch>/attack_A/jobs.txt
- <scratch>/attack_A/jobs2.txt
- <scratch>/attack_A/summ.py
- <scratch>/attack_A/table.py
- <scratch>/attack_A/regression_check.log
- <scratch>/attack_A/out/
- <scratch>/attack_A/out_preguard/
- <scratch>/attack_A/out_prefix2/
