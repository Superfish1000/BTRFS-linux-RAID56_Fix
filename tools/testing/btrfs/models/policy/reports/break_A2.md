# break_A2

**Verdict: A2 as written is not safe.** I found four ways it returns silently wrong data, and one general hole it does not close. All five sit between the drop and the taint: what counts as "missing-only", when the taint reaches disk, and when it is cleared. Each has a fix the model checks at 0 silent states.

With every fix applied, A2 is never silent in any run, and it keeps writing wherever strict A refuses because of missing-only records. It is still not loss-free, though: a whole-device taint loses acknowledged data (loudly) after a second, non-overlapping device loss, where strict A loses nothing. A per-stripe taint, resynced when the disk comes back, removes that loss in the model.

**Silent in A2 as specified (RAID5 unless noted, nodatasum):**
1. **A replace's own "zeros" record looks missing-only (7,650 states).** Replacing a missing device writes zeros where it can't rebuild, and records that. The record names the missing source's column, so A2 drops it and taints the source devid. The finished replace then clears the taint, and the zeros are served as data. The kernel's CUR already fails the replace when its own mark is spent (`replace_marks_lost`); A2 must keep that rule. With it: 0.
2. **The taint is not durable when the drop is.**
   - Taint written at the next commit: 3,456 silent states in the value model.
   - Persistence model: 91,111 (RAID5) and 589,033 (RAID6) mounts that lose both the record and the taint.
   - A superblock written under upstream's "one copy is enough" rule also loses it: 6,246 / 42,651.
   - Fixes, both 0 violations: put the taint in the header of every log block written while it holds, or require zero superblock errors before the drop. The second forbids drops while any present disk fails writes.
3. **A resumed scrub pass clears a taint set during the pass (12,030).** `btrfs scrub resume` keeps the position across a remount. Rule that fixes it: a pass clears only taints older than its start (0).
4. **The old disk returns in place of its replacement.** This is a general hole: OLD, A, CUR and CTM are all silent here, with no alert at all (1,392-1,500 states). A2's "replace clears the taint" adds 1,476 more. Fix: a tombstone with the superblock generation bound, for every replace whose source superblock was not scratched (0).

**Implementation checks.** The negative controls confirm that each of these is required:
- the drop must test every stale bit of the 64-block entry (3,590 silent otherwise);
- the taint must be honoured at all five read sites (5,428 to 25,392 silent if any one is missed);
- the lock-free `nr_stale` fast paths must count taints.

**What did not break:**
- RAID6 P/Q: 0 silent at depths 6-8.
- Verdicts: a verdict always names every present parity, so it never looks missing-only.
- Torn records never reach eviction in the model.
- Device scan and pull-then-replug: 0 silent.
- Deeper bounds (d10-d12): 0 silent.

**Loud costs compared with A:**
- **Two single-device absences in turn (never two missing at once).** A loses nothing and refuses 8,288 writes. A2, with the replace fix, loses 12,064 states (3,240 gone, 8,824 unreadable). 7,488 of those would read correctly with a per-stripe taint.
- **A second device dies after the return (W1).** A loses nothing; A2 loses 6,776.
- Cause: the dropped record was the only thing that told the return mount which stripe to repair.

**Wedging (writes before the first refusal):**
- A2 never refuses because of missing-only records. A refuses at write cap+1, which in the kernel is about 82 wide entries of 4 MiB (roughly 328 MiB of scattered degraded writes).
- A2 still refuses exactly like A in two cases: verdicts fill the log (a crash while degraded), or RAID6 has a present failing member.
- As modelled, A2 refused more than A after the disk returned (10/24 vs 17/24). Letting repair and mount recovery write onto a tainted disk fixes it (24/24).

**STUCK:** A2 is never STUCK where A is not. Without a spare, A is STUCK in 3,684 and 4,464 states; A2 in 0 and 3,200. A taint that can never be cleared only appears with a dead or failing disk and no spare, where no policy can restore redundancy anyway.

**Kernel finding (CUR):** a device scan of a returning disk clears MISSING while the disk stays unopened (volumes.c:959-962). CUR re-reads that flag on every mark, so its degraded eviction switches off and it refuses like A until remount (5/24 vs 24/24). A2 must test `bdev == NULL` instead.

**Recommended A2 (all fixes combined):** kernel replace rule, repair onto tainted disks, per-stripe taint cleared on rewrite, resync at the return mount, and the tombstone.
- 0 silent in all 22 families run.
- 0 lost and 0 refused where A loses 0 but refuses 8,288-10,512 writes (two absences, W1, RAID6 two absences).
- Equal to A in W2 when mount recovery also clears the taint: 4,748 gone / 908 unreadable.
- Remaining losses are classes A has too (the degraded write hole, and A-1).
- The per-stripe taint with resync is the design's stage-2 written-since-failure map applied to missing devices. The whole-device taint is its all-dirty case.

**How hard I pushed:**
- 113 value-model runs, depths 6-12, up to 1.03M states per run; 9 were cut short on memory or time and are marked T.
- 10 persistence-model runs at depth 12-14, up to 2.9M states.
- 8 regression pairs: the attack model with its extensions off reproduces policy_model.py exactly.
- A deterministic writes-before-refusal script.

**REPORT.md was not written:** the harness refuses report files from subagents, so the full report is this structured output. Scripts and per-run outputs are in `attack_A2/`.

## Table

## 1. Exhaustive value model (a2_model.py)

Each cell counts distinct states, except REF_WRITE and RO/down, which count transitions.
- **LOST gone:** the acknowledged value is on no disk.
- **LOST unreach:** the value is on disk, but no sequence of up to 3 admin actions reads it back.
- **taint-only:** a subset of LOST; every read would be right if the taint were removed.
- **TAINT_STUCK:** writes work, but no admin sequence clears the taint.
- **T:** the run was stopped on its memory or time limit.

Unless the row says otherwise: RAID5, nodatasum, 2 full stripes.

**What "recommended" means.** A2 plus all of these:
- `--replace-own fail`
- `a2_phaseA_tainted`
- `--taint-region 1` and `--taint-clear column`
- `--taint-resync`
- `--tombstone all`

| family / variant | states (depth) | SILENT | LOST gone | LOST unreach | of which taint-only | STUCK | TAINT_STUCK | REF_WRITE | RO/down |
|---|---|---|---|---|---|---|---|---|---|
| **Taint durability (missing_return_full, cap 1, d10)** | | | | | | | | | |
| A2 atomic (taint in the block that drops) | 84384 (d10) | 0 | 7120 | 22032 | (2112) | 0 | 0 | 98264 | 0 |
| A2 taint-first, zero sb errors | 84384 (d10) | 0 | 7120 | 22032 | 2112 | 0 | 0 | 98264 | 0 |
| A2 drop-first (taint at next commit) | 98498 (d10) | 3456 | 9232 | 24912 | - | 0 | 0 | 102848 | 0 |
| A2 drop-first, csum | 82970 (d10) | 0 | 8960 | 0 | 0 | 0 | 0 | 29424 | 0 |
| A2 atomic, pull+replay | 624641 (d5 T) | 0 | 46352 | 168612 | - | 0 | 0 | 575449 | 199447 |
| A2 drop-first, pull+replay | 660987 (d5 T) | 21021 | 53330 | 157133 | - | 0 | 0 | 547543 | 189837 |
| **Replace's own zeros record (two sequential absences, cap 1)** | | | | | | | | | |
| A (strict) | 5648 (d10) | 0 | 0 | 0 | 0 | 0 | 0 | 8288 | 0 |
| CUR (kernel replace rule) | 91758 (d10) | 57728 | 51224 | 3468 | 0 | 0 | 0 | 0 | 0 |
| A2 as specified (own record droppable) | 61414 (d10) | 7650 | 9732 | 9982 | 8070 | 0 | 0 | 50396 | 0 |
| A2 + kernel replace rule | 53764 (d10) | 0 | 3240 | 8824 | 7488 | 0 | 0 | 36500 | 0 |
| A2 + kernel replace rule, csum | 63982 (d10) | 0 | 3140 | 0 | 0 | 0 | 0 | 17064 | 0 |
| A2 + replace rule + per-stripe taint | 74160 (d10) | 0 | 3588 | 288 | 288 | 0 | 0 | 37668 | 0 |
| CTM (device-budget artifact) | 16305 (d10) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| A, crash 1 | 88716 (d9) | 0 | 22208 | 17400 | 0 | 0 | 0 | 176116 | 0 |
| A2 + replace rule, crash 1 | 580440 (d9) | 0 | 110272 | 170830 | 64598 | 0 | 0 | 667656 | 0 |
| A2 + replace rule, crash 1 (deep) | 625944 (d10) | 0 | 97172 | 206212 | 96688 | 0 | 0 | 818312 | 0 |
| A2 + replace rule, RAID6, 3 absences | 409278 (d8) | 0 | 2760 | 3196 | 2092 | 0 | 0 | 50476 | 0 |
| A | 5664 (d11) | 0 | 0 | 0 | 0 | 0 | 0 | 8672 | 0 |
| A2 recommended | 44788 (d11) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| **Scrub pass clearing the taint (detach 2, cap 1, d11)** | | | | | | | | | |
| A2 atomic scrub | 32896 (d11) | 0 | 1792 | 6400 | 5528 | 0 | 0 | 29452 | 0 |
| A2 resumable, epoch rule | 73640 (d11) | 0 | 4248 | 13398 | 11758 | 0 | 0 | 62392 | 0 |
| A2 resumable, no epoch | 107508 (d11) | 12030 | 15390 | 17422 | 14818 | 0 | 0 | 78862 | 0 |
| A2 per-stripe, epoch | 201546 (d11) | 0 | 5356 | 60548 | 55858 | 0 | 0 | 121704 | 0 |
| A2 per-stripe, no epoch | 204632 (d11) | 16688 | 18172 | 57386 | 52338 | 0 | 0 | 113514 | 0 |
| A2 resumable, epoch, crash 1 | 719747 (d9) | 0 | 125364 | 195810 | 85320 | 0 | 0 | 769854 | 0 |
| A2 recommended, resumable | 73756 (d11) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| **Old disk returns in place of its replacement (replaced while missing, d9)** | | | | | | | | | |
| OLD | 5354 | 1500 | 896 | 604 | 0 | 0 | 0 | 0 | 0 |
| A | 4447 | 1392 | 832 | 560 | 0 | 0 | 0 | 1536 | 0 |
| CUR | 11499 | 2868 | 1712 | 1156 | 0 | 0 | 0 | 0 | 0 |
| CTM | 5354 | 1500 | 896 | 604 | 0 | 0 | 0 | 0 | 0 |
| A2 (replace clears the taint) | 11499 | 2868 | 1712 | 1156 | 0 | 0 | 0 | 0 | 0 |
| A2 + tombstone for tainted devices | 13557 | 1392 | 832 | 560 | 0 | 0 | 0 | 0 | 0 |
| A2 + tombstone for every unscratched replace | 14351 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| A + tombstone for all | 4579 | 0 | 0 | 0 | 0 | 0 | 0 | 2880 | 0 |
| A2, csum | 9787 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| A2 recommended | 17651 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| **Old disk returns (replaced while failing writes, d9)** | | | | | | | | | |
| OLD | 9701 | 6734 | 4076 | 1906 | 0 | 0 | 0 | 0 | 0 |
| A / CUR / A2 / A2 + tombstone for tainted (each) | 4908 | 1224 | 702 | 522 | 0 | 0 | 0 | 1900 | 0 |
| A2 + tombstone for all | 7846 | 0 | 0 | 0 | 0 | 0 | 0 | 1900 | 0 |
| A2 recommended | 8670 | 0 | 0 | 0 | 0 | 0 | 0 | 1900 | 0 |
| **Entry of 2 stripes (3 stripes, 1-entry log, transient + missing)** | | | | | | | | | |
| A2 checks one stripe of the entry only | 376047 (d8) | 3590 | 9626 | 20784 | 11664 | 0 | 0 | 395644 | 0 |
| A2 recommended (every bit checked) | 402271 (d8) | 0 | 0 | 2344 | 2344 | 0 | 0 | 416824 | 0 |
| **Torn / verdict exclusions (replay pending, detach + return, cap 1, d9)** | | | | | | | | | |
| A | 15908 | 0 | 1024 | 5120 | 0 | 0 | 0 | 30592 | 6144 |
| CUR | 93458 | 33382 | 26702 | 18588 | 0 | 0 | 0 | 37360 | 0 |
| A2 = A2 dropping torn = A2 dropping verdicts (identical) | 86168 | 0 | 6352 | 26184 | 2112 | 0 | 0 | 95056 | 12136 |
| A2 recommended | 93992 | 0 | 6864 | 24104 | 0 | 0 | 0 | 80408 | 16568 |
| **RAID6, two sequential absences with return** | | | | | | | | | |
| A (crash 1, replace) | 274816 (d8) | 0 | 26744 | 70168 | 0 | 2286 | 0 | 517412 | 0 |
| CUR (crash 1, replace) | 914353 (d6 T) | 165968 | 126651 | 180501 | 0 | 1700 | 0 | 134990 | 0 |
| A2 (crash 1, replace) | 810072 (d6 T) | 0 | 46976 | 152732 | 5556 | 1446 | 0 | 263590 | 0 |
| A2 (crash 0, replace rule) | 185842 (d8) | 0 | 0 | 0 | 0 | 0 | 0 | 15400 | 0 |
| A2, 3 absences | 477942 (d8) | 0 | 1896 | 2620 | 2020 | 0 | 0 | 49588 | 0 |
| A2 recommended (crash 0) | 205932 (d8) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| A2 recommended (crash 1) | 584635 (d6) | 0 | 26464 | 66224 | 0 | 1996 | 0 | 164772 | 0 |
| **Taint window W1: a second device dies after the return (d9)** | | | | | | | | | |
| A | 7902 | 0 | 0 | 0 | 0 | 0 | 0 | 10512 | 0 |
| A2 | 45082 | 0 | 4288 | 2488 | 2488 | 0 | 0 | 12808 | 0 |
| A2 per-stripe + column clear (no resync) | 60492 | 0 | 9096 | 576 | 576 | 0 | 0 | 14528 | 0 |
| A2 recommended | 43598 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| A, no spare | 7902 | 0 | 0 | 0 | 0 | 3684 | 0 | 10512 | 0 |
| A2, no spare | 45082 | 0 | 4288 | 2488 | 2488 | 0 | 10552 | 12808 | 0 |
| A2 recommended, no spare | 43598 | 0 | 0 | 0 | 0 | 0 | 14668 | 0 | 0 |
| **Taint window W2: a present disk fails writes after the return (d9)** | | | | | | | | | |
| A | 23283 | 0 | 4748 | 908 | 0 | 0 | 0 | 42848 | 0 |
| A2 | 69215 | 0 | 10564 | 7660 | 4508 | 0 | 0 | 84320 | 0 |
| A2 + repair onto tainted | 66415 | 0 | 9172 | 8724 | 4908 | 0 | 0 | 83704 | 0 |
| A2 per-stripe + column clear | 77243 | 0 | 8972 | 908 | 0 | 0 | 0 | 90936 | 0 |
| A2 recommended | 62477 | 0 | 4820 | 2164 | 896 | 0 | 0 | 78392 | 0 |
| A2 recommended + recovery clears | 61149 | 0 | 4748 | 908 | 0 | 0 | 0 | 77384 | 0 |
| A, no spare | 23283 | 0 | 4748 | 908 | 0 | 4464 | 0 | 42848 | 0 |
| A2, no spare | 69215 | 0 | 10564 | 7660 | 4508 | 3200 | 7350 | 84320 | 0 |
| A2 recommended, no spare | 62477 | 0 | 4820 | 2164 | 896 | 976 | 9652 | 78392 | 0 |
| **Taint window W3: crashes after the return (crash 2, d9)** | | | | | | | | | |
| A | 98456 | 0 | 13312 | 30240 | 0 | 0 | 0 | 213088 | 0 |
| A2 | 521500 | 0 | 64096 | 187000 | 19708 | 0 | 0 | 685192 | 0 |
| A2 recommended | 557676 | 0 | 71880 | 154528 | 4212 | 0 | 0 | 618816 | 0 |
| **Pull the failing disk, replug it later (A-4's way out)** | | | | | | | | | |
| A | 275872 (d9) | 0 | 66820 | 44618 | 0 | 0 | 0 | 497968 | 0 |
| A2 | 580960 (d8 T) | 0 | 92902 | 150325 | 36875 | 0 | 0 | 721333 | 0 |
| A2 recommended | 707223 (d8 T) | 0 | 106923 | 131607 | 4960 | 0 | 0 | 987697 | 0 |
| **Device scan re-registers a missing disk (d9)** | | | | | | | | | |
| A2 (missing means bdev == NULL) | 125051 | 0 | 9952 | 27840 | 2112 | 0 | 0 | 115312 | 0 |
| A2 keyed on the MISSING bit | 125051 | 0 | 9952 | 27840 | 2112 | 0 | 0 | 160144 | 0 |
| **Negative controls: one taint enforcement site forgotten (missing_return_full, d10)** | | | | | | | | | |
| direct read (bio.c:543) | 84384 | 18684 | 6736 | 15952 | 0 | 4848 | 1616 | 98264 | 0 |
| RMW / rebuild source (raid56.c:2060) | 109724 | 20148 | 30396 | 22824 | 2488 | 0 | 0 | 106920 | 0 |
| mount recovery (scrub.c:5385/5122) | 79344 | 5428 | 12148 | 14304 | 0 | 0 | 0 | 69288 | 0 |
| scrub (scrub.c:931, 3371) | 96404 | 13236 | 20100 | 19152 | 0 | 0 | 0 | 106808 | 0 |
| replace source (scrub.c:2716) | 111696 | 25392 | 20028 | 28052 | 0 | 768 | 0 | 124024 | 0 |
| replace source, device present (d9) | 7676 | 1216 | 632 | 584 | 0 | 0 | 0 | 1328 | 0 |
| no taint at all | 84184 | 24320 | 21212 | 19428 | 0 | 0 | 0 | 72192 | 0 |
| per-stripe taint, read site forgotten | 114560 | 20772 | 10152 | 22768 | 0 | 1320 | 440 | 115496 | 0 |
| **Deeper bounds, A2 + kernel replace rule** | | | | | | | | | |
| missing_return_full | 84752 (d12) | 0 | 7120 | 22064 | 2112 | 0 | 0 | 100352 | 0 |
| cap2_missing_return | 84326 (d9) | 0 | 0 | 0 | 0 | 0 | 0 | 4512 | 0 |
| replay_full | 12764 (d11) | 0 | 0 | 512 | 0 | 0 | 0 | 14080 | 1920 |
| **Recommended A2 in other families** | | | | | | | | | |
| missing_return_full (A: 14184 states, 0 / 1312 / 2912, REF 28032) | 90196 (d11) | 0 | 8128 | 17120 | 0 | 0 | 0 | 82584 | 0 |
| fresh values | 180246 (d10) | 0 | 8120 | 17104 | 0 | 0 | 0 | 81600 | 0 |
| two absences, crash 1 | 451604 (d9) | 0 | 97652 | 72880 | 0 | 0 | 0 | 430924 | 0 |
| two absences, csum | 37900 (d10) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| replay_full | 12634 (d10) | 0 | 0 | 512 | 0 | 0 | 0 | 14072 | 1920 |
| flush + missing | 546929 (d7 T) | 0 | 65590 | 179084 | 32 | 0 | 0 | 568960 | 0 |
| cap 2, 3 stripes, crash 1 | 1029663 (d7 T) | 0 | 47712 | 98614 | 0 | 0 | 0 | 121736 | 0 |
| metadata + missing | 83252 (d9) | 0 | 15472 | 0 | 0 | 0 | 0 | 24096 | 24096 |
| device scan | 146231 (d9) | 0 | 11056 | 22960 | 0 | 0 | 0 | 103728 | 0 |

## 2. Where must the taint live? (tp_model.py, depth 14)

A **violation** is a mount at which neither the dropped record nor the taint is known, so the returning device's stale column is trusted. Count per run, RAID5 (3 devices) / RAID6 (4 devices):

| placement | RAID5 states | RAID5 violations | RAID6 states | RAID6 violations |
|---|---|---|---|---|
| log-header: in every log block written while the taint holds, so it is atomic with the drop | 604,883 | **0** | 1,718,587 | **0** |
| sb-first-all: superblock with zero errors before the drop | 623,971 | **0** | 2,282,221 | **0** |
| sb-first: superblock under upstream's one-copy rule, then the drop | 717,985 | 6,246 | 2,619,040 | 42,651 |
| sb-lazy: drop at the mark, superblock at the next commit | 916,232 | 91,111 | 2,905,904 | 589,033 |
| log-once, control: taint only in the first block after the drop (depth 12) | 261,216 | 30,598 | 752,427 | 329,987 |

## 3. Writes acknowledged before the first refusal (wedge.py)

8 full stripes, and each write goes to a new region. A cell reads "acknowledged/attempted, first refusal at #n".

| scenario | cap | A | CUR | A2 | A2 taint-first | A2 + repair onto tainted | A2 per-stripe | A2 keyed on MISSING bit | CTM |
|---|---|---|---|---|---|---|---|---|---|
| degraded | 1 | 3/24 first@2 | 24/24 | 24/24 | 24/24 | 24/24 | 24/24 | 24/24 | 24/24 |
| degraded | 4 | 12/24 first@5 | 24/24 | 24/24 | 24/24 | 24/24 | 24/24 | 24/24 | 24/24 |
| degraded + crash (verdict) | 1 | 0/25 first@2 | 24/25 | 0/25 first@2 | 0/25 | 0/25 | 0/25 | 0/25 | 0/25 |
| degraded + crash | 2 | 4/25 first@3 | 24/25 | 24/25 | 24/25 | 24/25 | 24/25 | 24/25 | 24/25 |
| degraded + 2 crashes | 2 | 0/26 first@3 | 24/26 | 0/26 first@3 | 0/26 | 0/26 | 0/26 | 0/26 | 0/26 |
| RAID6 missing + present failing | 2 | 4/24 first@3 | 23/24 | 4/24 first@3 | 4/24 | 4/24 | 4/24 | 4/24 | 24/24 |
| missing disk returned | 1 | 17/24 first@2 | 24/24 | 10/24 first@9 | 10/24 | 24/24 | 24/24 | 10/24 | 24/24 |
| missing disk re-appears to a device scan | 1 | 4/24 first@2 | 5/24 first@3 | 24/24 | 24/24 | 24/24 | 24/24 | 5/24 first@3 | 24/24 |

## Counterexamples

### A2 (as specified): SILENT_WRONG (7,650 states; record_dropped alert raised earlier, but the read itself carries no error) (silent wrong read; the fix gives 0 (R_twice_A2_fail))

Scenario: Two single-device absences in turn, 1-record log, replace of the second (family R_twice, depth 10)

Trace: d0 detaches || write s0.d0=2 ok || write s1.d1=2 evict s0[0] taint{d0} ok || unmount ; mount with {d0} back || d1 detaches || replace d1  => inside the replace, s0.d1 cannot be rebuilt (d0 is tainted, P is the only parity), so the replace writes zeros and records them as {d1}. The next stripe's zeros need the slot, so the s0 record, which names only the missing d1, is evicted with taint{d1}, and the finished replace clears d1's taint. Result: s0.d1 reads the zeros (-9, acknowledged 1) and s0.d0 reads a garbage rebuild (-17, acknowledged 2).

Kernel: A2 is not implemented. CUR already has the protection that A2's rule must keep:
- A replace's own marks (replace_stale/replace_keep bits) are spent only in pass 2 (wib_entry_replace_owned(), wib_evict_sticky() at raid56-wib.c:801-901).
- Spending one sets replace_marks_lost (901), and btrfs_wib_replace_end() then fails the replace (4037-4043).
- After a remount the marks look like ordinary naming records; btrfs_wib_replace_resume_rewinds() (4108) makes the resumed replace copy again from the start.
A predicate "every stale bit is on a device with bdev == NULL" classifies these marks as droppable, because they name the missing source's column, unless it also excludes wib_entry_replace_owned(). Separately, a replace may clear the source's taint only if that taint predates the replace.

### A2, taint persisted at the next commit (drop-first): SILENT_WRONG (3,456 at d10; 21,021 in pull+replay at d5) (silent wrong read (alert latched before the crash, lost with it))

Scenario: missing_return_full: the log block omitting the record reaches disk at the mark, the taint only at the op's commit

Trace: d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] taint{d0}(not durable) ; CRASH after mark ; mount with {d0} back  => s0.d0 reads 1 (acknowledged 2)

Kernel: Applies to any implementation that keeps the taint in the superblock or a dev item. btrfs_wib_mark() writes the block that omits the evicted record synchronously (wib_write_block_locked(), raid56-wib.c:3262, via wib_write_all_devices() with FUA). Superblocks are written only by write_all_supers() at the commit.
Fix: carry the taint in struct btrfs_wib_disk_header (flags plus reserved[6], raid56-wib.h:186-199) of every block written while it holds. An unknown header flag makes older stage-0 kernels reject the block (BTRFS_WIB_FLAGS_SUPPORTED, 221-228). The persistence model gives 0 violations for this placement.

### A2, taint in the superblock under upstream's one-copy rule (sb-first): lost taint and record: 6,246 violating mounts (RAID5) / 42,651 (RAID6), tp_model depth 14 (silent wrong read at that mount (needs a transient error plus P absent at the next mount))

Scenario: Superblock copies disagree after a transient write error

Trace: d1 starts failing writes || taint X in the superblocks (only P takes it; write_all_supers still succeeds) then drop R || d1 heals || log block 2 (without R) || CRASH || mount with {d0,d1}  => the newest block on the present devices lacks R, the newest present superblock (d1's) lacks the taint, and X=d0's stale column is trusted

Kernel: write_all_supers() accepts a commit if at least one superblock landed (max_errors = num_devices - 1). The design's stage-1 rule (zero superblock errors) and the log's own copy rule (raid56-wib.c:1902-1956) both give 0 violations. Zero superblock errors also blocks every drop while a present disk fails writes: in the wedge table, A2 taint-first refuses like A in RAID6.

### A2, scrub clears whatever is tainted when a pass ends (no epoch): SILENT_WRONG (12,030 whole-device; 16,688 per-stripe) (silent wrong read; the epoch rule gives 0)

Scenario: Resumable scrub interleaved with a detach, degraded writes and a remount

Trace: scrub start s0 || d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] taint{d0} ok || unmount ; mount with {d0} back || scrub step s1 (pass ends, clears taint)  => s0.d0 reads 1 (acknowledged 2)

Kernel: Not implemented. `btrfs scrub resume` restarts the kernel ioctl at a saved offset. A rule like "clear the taint when a scrub of the device finishes without uncorrectable errors" would accept the resumed pass. A pass may clear only taints older than its start, and it must cover the whole device within that epoch.

### OLD, A, CUR, CTM and A2 (general hole); A2's replace-clears-taint adds to it: SILENT_WRONG with no alert at all (missing case: A 1,392, OLD/CTM 1,500, CUR/A2 2,868; failing case: 1,224 for A/CUR/A2, 6,734 for OLD) (silent, no alert; a tombstone for every replace whose source superblock was not scratched gives 0 (A and A2))

Scenario: A device replaced while missing (or while failing writes); later the old disk is found in place of its replacement

Trace: d0 detaches || write s0.d0=2 ok || replace d0 || unmount ; mount with the old d0 instead of its replacement  => s0.d0 reads 1.
A2-specific: d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] taint{d0} ok || replace d0 || unmount ; mount with the old d0 ...  => s0.d0 and s1.d0 read 1.
Failing case: d0 starts failing writes || write s0.d0=2 ok (write to {d0} failed) || replace d0 || unmount ; mount with the old d0 ...

Kernel: Applies to upstream and the current kernel:
- dev-replace.c:1017-1019 swaps the uuids, so the target carries the source's devid and uuid.
- dev-replace.c:1058-1059 scratches the source superblock only if it is WRITEABLE.
- device_list_add() (volumes.c:760-962) accepts the old disk for that devid whenever the replacement is absent.
The stage-1 design's tombstone/witness rule (sb_bound) covers FAILED devices only; it should cover every replace of a missing or write-failing source. Also key the taint by devid plus uuid: find_next_devid() (volumes.c:2015) reuses max+1 after a remove.

### A2, drop test on one stripe of a multi-stripe entry (control): SILENT_WRONG 3,590 (silent; checking every record of the entry gives 0)

Scenario: An entry covering 2 stripes; one names the missing d1, the other names the present d0 from an earlier failed write

Trace: d0 starts failing writes || write s1.d0=2 ok (write to {d0} failed) || d0 heals || d1 detaches || write s0.d1=2 ok || write s2.d0=2 evict e0{s0[1],s1[0]} taint{d1} ok  => s1.d0 reads 1 (acknowledged 2)

Kernel: Kernel entries cover 64 blocks, with stale and stale_par per block (raid56-wib.h:250-298). The A2 predicate must map every stale|stale_par bit to its device through the chunk map. It must also require !bitmap, !torn, !verdict (suspect_par/prior_par) and !replace-owned.
Eviction runs under wib->lock (lockdep at raid56-wib.c:803), and every stale-bit setter runs under it too (after lock acquisitions at 2614, 2939, 3810, 3924, 3970, 4177, 4224). So an all-bits check is atomic against a name added to the same entry.

### A2 (with the kernel replace rule): LOST_ACKED 12,064 (3,240 gone + 8,824 unreadable; 7,488 of these only because of the taint's granularity), where strict A has 0 (loud loss (EIO), which the user's rule counts as losing data. Per-stripe taint plus resync at the return mount gives 0 lost and 0 refused (F_twice_A2fin, F_W1_A2fin))

Scenario: Two single-device absences in turn, never two missing at once (d10); W1: a second device dies after the return (A2 6,776 lost, A 0)

Trace: Gone: d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] taint{d0} ok || unmount ; mount with {d0} back || P detaches || write s1.d0=3 ok  => s0.d0's acknowledged 2 lived only in P.
W1: d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] taint{d0} ok || unmount ; mount with {d0} back || d1 dies  => all four cells EIO.
Under A the evicting write is refused, and the return mount's recovery repairs s0.d0 from P.

Kernel: Applies to any whole-device taint: the dropped record was the only thing that told the return mount's recovery (scrub_raid56_plan_wib() and scrub_raid56_recover_absent(), scrub.c:3357/5385) which stripe to rewrite. A per-region taint resynced at re-add is the stage-2 written-since-failure map (design section 3.8, T-missing) applied to missing devices; the whole-device taint is its all-dirty case.

### A2 (whole-device taint): LOST_ACKED_UNREACHABLE caused only by the taint (LOST_TAINT_ONLY: 2,112 in missing_return_full; 2,488 in W1; 4,508 in W2) (loud loss of data that is physically intact)

Scenario: The returned disk holds the correct value, but the taint forbids reading it and no other copy exists

Trace: d0 detaches || write s0.d0=2 ok || write s1.d1=2 evict s0[0] taint{d0} ok || unmount ; mount with {d0} back || write s0.d0=3 ok || d1 dies  => s0.d0=3 is on d0's platter, but d0 is tainted and d1 is gone: EIO forever

Kernel: Inherent to a device-wide taint. A per-stripe/region taint cleared when the column is rewritten (from a trusted rebuild, a full write, or mount recovery) removes it: LOST_TAINT_ONLY 0 in F_W2_A2fin_rc and F_mrf_A2fin.

### A2 (model's A2: no repair write-back onto a tainted disk): REFUSED_WRITE after the disk returns: 10/24 acknowledged vs 17/24 for A (1-record log), first refusal at write 9 (availability (A2 worse than A); letting phase A and mount recovery write a rebuilt value onto the tainted disk gives 24/24)

Scenario: 8 writes into d0's column while it is missing, d0 returns, then 16 writes into d1's column

Trace: (wedge.py 'returned') after 'mount with {d0} back': #9 write s0.d1=2 REFUSED(log-full) ... The log holds records naming the now-present, tainted d0. Mount recovery treats d0 as absent and cannot repair them, and phase A skips tainted columns, so the records never retire until a scrub.

Kernel: An implementation choice for A2. The taint must mean "untrusted for reads", not "excluded from writes": rmw_repair_first() (raid56.c:4675) and mount recovery must still write a rebuilt column onto a tainted device.

### A2 (and A): REFUSED_WRITE (wedge) that remains (availability: A2 still refuses where A does in these two cases; CUR keeps writing only by evicting verdicts or present names (CUR-2/CUR-5))

Scenario: (a) Verdicts fill the log after a crash while degraded (nodatasum); (b) RAID6 with d0 missing and a present member d1 failing writes

Trace: (a) degraded + crash, cap 1: #1 write s0.d1=2 ; phaseB landed {P} ; CRASH ; mount (verdict TV[2] on s0), then every write into a new region is REFUSED(log-full): 0/25 for A and A2, 24/25 for CUR.
(b) records name the present failing d1, so they are not droppable: A2 = A (2/24, 4/24, 8/24 at caps 1/2/4).

Kernel: Kernel: btrfs_wib_mark() fails at WIB_ROOM_NONE (raid56-wib.c:3636). The wide log holds 82 entries of 4 MiB ((4096-128)/48). A crash with many stripes in flight on a degraded mount can therefore fill the log with verdicts, and A2 then refuses every new-region write until the missing device returns or is replaced.

### CUR (kernel today), and A2 if keyed on the MISSING bit: REFUSED_WRITE: 5/24 acknowledged (CUR) vs 24/24 (A2 keyed on bdev == NULL), 1-record log (availability wedge (safe, but surprising: plugging the disk back in stops writes))

Scenario: A missing disk is plugged back while mounted; udev's device scan re-registers it, but it stays unopened

Trace: d0 missing at mount; 2 writes into d0's columns; d0 re-appears (device scan, no I/O until a mount) => every further write into a new region is REFUSED(log-full) until a remount

Kernel: Applies to the current kernel:
- device_list_add() decrements missing_devices and clears BTRFS_DEV_STATE_MISSING while bdev stays NULL (volumes.c:959-962).
- wib_may_evict_naming() reads missing_devices (raid56-wib.c:724), and wib_policy_locked() refreshes it on every mark (944, 1118, 1148), so CUR's degraded exception turns off at once.
A2 must define "missing" as bdev == NULL.

### A2 with one taint enforcement site forgotten (controls): SILENT_WRONG: read 18,684; RMW/rebuild 20,148; mount recovery 5,428; scrub 13,236; replace source 25,392 (present source 1,216) (each site is necessary)

Scenario: missing_return_full, d10

Trace: Recovery: d0 detaches || write s0.d0=2 ok || write s1.d0=2 evict s0[0] taint{d0} ok || write s0.d0=3 evict s1[0] taint{d0} ; CRASH after mark ; mount with {d0} back  => recovery recomputes P from the stale d0 and destroys the acknowledged value.
Scrub: ... evict s0[0] taint{d0} ; CRASH after mark ; mount with {d0} back || scrub.
RMW: write s0.d0=2 ; CRASH after mark ; mount without d0 || write s0.d0=3 ok || write s1.d1=2 evict s0[0] taint{d0} ok || unmount ; mount with {d0} back || write s0.d1=2 ok.

Kernel: Sites:
- direct read: bio.c:543, gated by btrfs_wib_any_stale();
- RMW/rebuild source: mark_stale_sectors(), raid56.c:2060;
- mount recovery: scrub.c:3357/5385/5122;
- scrub: scrub.c:931 (nodatasum trust) and 3371;
- replace copy source: scrub.c:2716 (trust_source).
btrfs_wib_any_stale() and btrfs_wib_stale() have a lock-free nr_stale == 0 fast path (raid56-wib.c:3843, 4712). A drop can bring nr_stale to 0, so a taint must keep those gates open, or every site skips it.

## Sensitivity

**Regression.** With every extension off, a2_model.py (md5 f67e970d) reproduces policy_model.py (md5 3ed8fa4f) exactly: 8/8 identical RESULT lines across A, A2, CUR and CTM, RAID5 and RAID6, nodatasum and csum (out_regress.log). The only difference is the new metric fields, which are stripped for the comparison.

**Value-model controls. Each must show its failure class, and all do:**
- **Taint ordering:** drop-first gives SILENT 3,456 (atomic and taint-first: 0).
- **Replace's own record:** droppable gives SILENT 7,650 (kernel rule: 0).
- **Scrub epoch:** without it, SILENT 12,030 whole-device and 16,688 per-stripe (with it: 0).
- **Old disk after replace:** no tombstone gives 1,392-2,868 SILENT, with no alert in 1,392 of them (tombstone for all: 0).
- **Entry drop checking one stripe only:** SILENT 3,590 (all records checked: 0).
- **A taint site forgotten:** direct read 18,684; RMW/rebuild source 20,148; mount recovery 5,428; scrub 13,236; replace source 25,392; replace source with the device present 1,216; per-stripe taint with the read site forgotten 20,772.
- **No taint at all:** SILENT 24,320, which reproduces RESULTS.md's `a2_no_taint` control.
- **CUR in the new families:** silent everywhere, as RESULTS predicts: 57,728 (two absences), 33,382 (replay), 165,968 (RAID6, truncated).

**Controls that did not bite, and why:**
- `a2_drop_torn` and `a2_drop_verdict` give results identical to plain A2 in every family tried: missing_return_full, crash_loss with crash 2, replay pending, RAID6 at depth 6.
- A verdict always names every present parity (scrub_raid56_mark_suspect: "every present parity stale"), so it is never missing-only.
- Mount recovery resolves every torn record before any eviction. The model has no crash that leaves both a torn record and a pending tree-log replay.
- So these two exclusions are argued rather than demonstrated. In the kernel, the torn case is reachable through CUR-3's path: error records wait for the replay (raid56-wib.c:7544, 7575-7577).

**Persistence-model (tp_model) control:**
- A taint written only in the first block after the drop: 30,598 (RAID5) / 329,987 (RAID6) violations at depth 12.
- The correct log-header placement: 0.
- The "one superblock copy is enough" and lazy placements bite (6,246 / 42,651 and 91,111 / 589,033).

**Metric check (LOST_TAINT_ONLY):** it is set only when every read would be right with the taint removed. It is 0 in every non-A2 run and in the per-stripe variants that clear the taint on rewrite (e.g. R_twice_A2P_fail 288 vs 7,488).

## Caveats

**No REPORT.md.** The harness refuses report files from subagents, so this structured output is the report. Scripts, job lists and every run's output (RESULT line, shortest traces per class, first trace per metric) are in attack_A2/ and attack_A2/out/.

**Value model (a2_model.py) abstractions,** mostly the same as policy_model.py:
- One sector per column, 2 data columns, 2-3 full stripes.
- Replace and scrub are atomic, except in `--scrub-mode cursor`.
- One operation at a time; admin closure depth 3.
- RAID6 Q is a snapshot.
- Log capacity is counted in records, or in entries with `--entry-stripes`.
- State counts are distinct states and depend on how large each policy's state space grows. Compare zero against non-zero, and compare traces; do not compare raw magnitudes across policies (e.g. W3: A 98k states vs A2 521k).
- The replace-own "fail" rule models the kernel's replace_marks_lost as "the replace does nothing". A resumed replace after a remount (btrfs_wib_replace_resume_rewinds) is argued, not modelled.

**Persistence model (tp_model.py) abstractions:**
- One dropped record R and one tainted device X.
- Per-device newest log block (seq) and superblock (generation), with transient write errors, crashes, clean unmounts and any tolerated device set at mount.
- The superblock rollback (mounting an older generation) is modelled only as "which taint list the mount sees". The loss of the rolled-back transactions is an upstream property and is not counted.

**Old-disk (ghost) event:** one event per run. The replacement is treated as gone for good when the old disk appears.

**Not modelled:**
- Concurrency within the kernel (the lock argument is from the code).
- Runtime return of a missing device: the kernel keeps it unopened, and the model returns devices only at a mount.
- Older kernels mounting a tainted filesystem. The log's feature is COMPAT_RO (btrfs.h:326), so an upstream read-only mount ignores both records and taint. The taint should set an INCOMPAT bit while any device is tainted, as the design does for the stage-1 list.
- Delete/free of an extent, so a stripe with an unreadable verdict can never be written again by the application.

**"Recommended A2" (F_ runs) is a model configuration, not a kernel design.**
- Per-stripe taint plus resync at mount stands for a region bitmap like the stage-2 map. Its real cost (resync time at region granularity, bitmap persistence under the log's copy rule, and the design's ordering R6 "nothing is dropped until the map is durable") is not in the model.
- The whole-device atomic resync in the A2+resync probe is unrealistic: a full scrub takes hours. That probe is not used as evidence.
- `--recovery-clears` was added last and ran only for W2 (F_W2_A2fin_rc).

**Budget and bounds:**
- 9 runs were cut short on the 2.5 GB memory limit or the time limit, and are marked T: P_pull_replay_* (d5), M_raid6_ret_A2/CUR (d6), X_pull_A2 and F_pull (d8), F_flush and F_cap2 (d7). Their counts are exact up to the depth reached.
- The machine was shared with the sibling attackers (load 6-7). To keep runs bounded I dropped these planned jobs: W6 variants, D_raid6_bmf_A2_d8, D_meta/D_flush for A2 as specified, the Z_ group (superseded by F_), M_entry for A and CUR, and CUR/A2pA in W1/W3.

**Model semantics to note:**
- CTM's zero loss in two-absence families is the device-budget artifact noted in RESULTS.md: a returned disk stays FAILED, so a second absence exceeds tolerance.
- The kernel capacity figure (82 wide entries of 4 MiB) comes from raid56-wib.h:86-199. It is an inference; nothing was run on a kernel.
- None of the counterexamples was reproduced on a UML kernel, and A2 does not exist in the kernel. Kernel lines are cited at BTRFS-s0 stage0-wip 2a1e2379e4.

## Model files

- <scratch>/attack_A2/a2_model.py
- <scratch>/attack_A2/tp_model.py
- <scratch>/attack_A2/wedge.py
- <scratch>/attack_A2/policy_model.orig.py
- <scratch>/attack_A2/run_jobs.sh
- <scratch>/attack_A2/run_regress.sh
- <scratch>/attack_A2/jobs.sh
- <scratch>/attack_A2/summ.py
- <scratch>/attack_A2/final_table.py
- <scratch>/attack_A2/final_table.md
- <scratch>/attack_A2/wedge.md
- <scratch>/attack_A2/wedge.verbose.txt
- <scratch>/attack_A2/tp_model.out
- <scratch>/attack_A2/out_regress.log
- <scratch>/attack_A2/out/
- <scratch>/attack_A2/out_probe_fin_mrf.txt
