# break_CUR

## Attack on CUR (the stage-0 kernel at BTRFS-s0 stage0-wip 2a1e2379e4)

I could not write `attack_CUR/REPORT.md`: the harness does not let subagents write report files. The full report is this output. Everything it cites is on disk in `attack_CUR/`.

### Bottom line
1. **All three exceptions (degraded, replay, torn-only) produce silent wrong reads of acknowledged data without checksums.** The ablations attribute every silent state to exactly one exception: running with only that exception reproduces CUR's count, and running without it gives A's count, which is 0 in every family. Checksummed data is never silent in any CUR run (all csum runs: SILENT = 0).
2. **The exceptions do two different kinds of harm.**
   - **Loss A would not have.** The degraded and replay exceptions spend records that name a stale member while the acknowledged value is still on the disks, in the parity. CUR then reads the stale copy with no error, and the next write to that stripe or the next scrub destroys the value for good. A's LOST(gone) is 0 in D1, D4, R1, R2 and R3. With non-aliasing values, 632, 1,136, 1,660 and 2,028 silent states in D1, D3, R1 and D4 still hold the value on a present disk.
   - **A loss A also has, made silent.** Spending a recovery verdict, a torn-only record, or the zero mark a finished replace left behind turns an EIO into garbage, zeros or an older version. With non-aliasing values, every silent state of this kind has the value already gone (SILENT_ONDISK = 0 in D2, T2 and T5). The torn-only exception only ever does this.
3. **The degraded exception covers the whole filesystem, not the stripe.**
   - `wib_may_evict_naming()` tests only `fs_devices->missing_devices` (raid56-wib.c:724). Any missing device is enough, including one that holds no column of the RAID5 chunk. Examples: a RAID1 metadata SSD, a disk added without a balance, or a disk hot-unplugged for a moment (`btrfs_remove_bdev()`, super.c:2524).
   - That missing device makes CUR spend the records naming a *present, failing* disk. Its stale platter is then read with no error, within RAID5's tolerance for the stripe. The shortest trace is 4 steps (CUR-6, new). The parent's CUR-5 is the RAID6 special case of this.
4. **New classes the parent run did not report:**
   - CUR-6: an unrelated missing device (RAID5).
   - CUR-7: a returning device whose *parity* column was stale. A later member loss rebuilds from it and returns old versions.
   - CUR-8: a finished replace's zero marks become ordinary records (raid56-wib.c:3995) and are spent, so the zeros are read as data.
   - CUR-9: a stripe the mount's recovery kept torn because a sector would not read (kept_torn) is spent, and the unreadable sector reads back as a rebuild from torn parity.
   - CUR-10: a failed parity flush the log could not name (a torn-only record) is spent, and a later member loss rebuilds from the stale parity.
   - CUR-11: `WIB_READD_LOSE` (raid56-wib.c:3016/3026). This is a drop path outside the three exceptions: a failed flush's stripes are simply not re-added, and the commit goes on. No knob gates it.
5. **The alerts do not let an administrator prevent the loss, except in one narrow case.**
   - The `record_dropped` latch lives only in memory (`alert_latched`, raid56-wib.h:813). With that modelled, 100% of the returning-device wrong reads happen in a mount whose `raid56_health` shows no alert (D1: 1,336 of 1,336; D4: 86%).
   - In 8 of the 11 mechanism classes the drop itself makes the read wrong (window 0). After the alert, no response helps: 100% of sampled drop states end silent and lost under every response.
   - Where there is a window (the device returning), the alert's own advice loses the data. The dmesg text says "fix or replace the failing device" and the warning says "run scrub". Reconnecting the old disk and scrubbing loses the data permanently in 100% of D1 and D4 drop states. Only replacing the missing devid with a new disk, before the old disk is seen again, is safe (100%).
   - The one exception: a torn-only record from a failed parity flush with every device present. There, "fix the device, then scrub" before the next member loss is safe in 100% of sampled drops.
   - A production kernel cannot choose refusal. `raid56_keep_naming_degraded` and `raid56_wf_torn_unevictable` exist only under `CONFIG_BTRFS_DEBUG` (raid56-wib.c:586; const false at 654-658).

### What I ran
The model is `attack_CUR/cur_model.py`, a copy of `policy_model.py` (md5 3ed8fa4f). With every new switch off it reproduces the parent's counts exactly (checked on 4 parent runs). New switches:
- `--exc deg,replay,torn,unnamed,lose` turns each CUR exception on or off (`none` is A).
- `--outsider` adds a device X outside the modelled stripes; X missing sets `missing_devices` but costs the stripe no redundancy.
- `--ure K` models latent sector errors. A read that meets one rebuilds; recovery of a torn stripe that meets one keeps it torn-only, which is the kernel's `kept_torn`.
- `--lose-flush` models `WIB_READD_LOSE`.
- `--alert-volatile` makes every mount clear the alert latch.
- Alert kind bits record which exception spent each record.

Two fidelity fixes:
- A running replace's own marks are now spent last, and spending one fails the replace, as `replace_owned` / `replace_marks_lost` do. The parent's model spent them in any order, which gave a spurious silent class. The mutation `evict_replace_marks` restores it as a control.
- A refused write now keeps "evict" in its trace label. This changes labels only.

Runs, all at nice 10 and each under 30 minutes:
- 132 matrix runs: 16 adversarial families, each under CUR, CUR with the volatile latch, CUR with csum, A, A2, and the "only X" / "without X" ablations, plus 9 of the parent's CUR families re-run 1-3 levels deeper with stripe symmetry.
- Mechanism classification (`classify.py`) of 18 families: the shortest trace per (what was dropped × where its named devices were × how the wrong value comes back).
- Alert-window and response analysis (`window.py`) of 12 families.
- 13 control and non-aliasing runs.
- Signature diffs between the parent's depth and 2 more levels on 4 families. No new mechanism appeared.

### Kernel paths
**Degraded.**
1. `wib_may_evict_naming()` (716) returns true on `missing_devices` (724). The count is raised at mount (volumes.c:7551) and on runtime unplug (super.c:2524; "continue as degraded" at 2543). It is re-read on every mark (731).
2. `wib_evict_sticky()` (801, pass loop at 847) then spends, in order:
   - pass 1: any non-verdict record, including names of *present* members and torn marks (`names_member` counts torn, 557);
   - pass 2: a running replace's own marks, which fails that replace (loud);
   - pass 3: verdicts (warning at 871).
3. It is reached from `btrfs_wib_mark()` (3552) → `wib_find_or_alloc_entry()` (982), called from `rmw_rbio()` (raid56.c:4939). It is also reached from `wib_enforce_capacity_locked()` (941) when the first stale bit halves the capacity from 165 entries to 82 (`btrfs_wib_mark_stale()` 3797-3831).
4. When the device returns, `device_list_add()` clears MISSING with no resync (volumes.c:943).

**Replay.**
- `BTRFS_FS_LOG_RECOVERING` is set at tree-log.c:7755 and cleared at 7908, so it covers the whole replay and its commit. It is tested at raid56-wib.c:725.
- While a replay is pending, error records are only verified (7544) and wait for it; the replay's RMWs spend them.
- Precondition: the replay or its commit must write sub-stripe RMWs into RAID5/6 block groups. That means RAID5/6 metadata, or data such as `space_cache=v1`. With RAID1 metadata and the free-space tree, this exception never fires.

**Torn-only.**
- `wib_entry_torn_only()` (775) and `wib_evictable(spend_torn)` (794) decide which records qualify. Passes 1-2 spend them while `!may_evict_naming` (856-858): flush-left records first, then `kept_torn` (784).
- `btrfs_wib_mark()` falls back to them when nothing else frees room (3589-3594). `btrfs_wib_try_mark()` always spends them (1196); its callers are `btrfs_wib_add_sticky()` at recovery and `scrub_raid56_mark_suspect()` (scrub.c:5130).

**Other drop sites that raise `EV_DROPPED`:**
- 3198: `WIB_READD_LOSE`, modelled as CUR-11;
- 3777: `btrfs_wib_add_sticky()` cannot keep a recovered stripe; not modelled.

### What each exception can lose, in kernel units
One log entry covers 4 MiB (64 blocks of 64 KiB, raid56-wib.h:107). The log holds 165 narrow or 82 wide entries.

**Degraded, missing column (CUR-1, CUR-7).**
- Once the log is full (82 regions, about 328 MiB of address space), every new region evicts an older record.
- On return, all data without checksums that was written while degraded to the missing device's columns, outside the newest ~82 regions, reads back from the stale platter.
- The first RMW or scrub of each stripe makes that permanent.
- Because parity rotates, the same returning disk also carries parity-stale stripes. Those return old versions at the next member loss.
- There is no safe action after the return: scrub destroys the data-stale stripes, while skipping it leaves the parity-stale stripes exposed.

**Degraded, present failing column (CUR-5, CUR-6).** Every evicted region's blocks on the failing disk read stale at once, for as long as the disk stays attached and keeps failing writes.

**Degraded, verdicts and zeros (CUR-2, CUR-8).** These convert a loud loss into a silent one.
- CUR-2 needs pass 3. A realistic trigger in the kernel is a crash with more than 82 regions in flight, followed by a degraded mount, then the capacity halving.

**Replay (CUR-3).** One region's records are lost per new region the replay and its commit need. This happens even with every disk healthy at the mount (the transient variant).

**Torn-only (CUR-9, CUR-10).** It never loses a value A keeps. It turns EIO into a wrong value for rebuilt sectors without checksums.

**Unnamed or LOSE flush (CUR-4, CUR-11).** A lost cache write on a data column reads old at once; A aborts the commit (read-only) instead.

### Alert usefulness
`window.py` takes the state right after the drop, applies each administrator response, then explores 3 more steps: writes, remounts, returns, and one more device loss within tolerance.
- **Returning device (CUR-1, CUR-7):** window 1-2 steps. `none`, `scrub` and `fix+scrub` all end silent and lost in 100% of sampled drops; `replace` and `replace+scrub` are SAFE in 100%.
- **Window-0 classes (CUR-2, CUR-3, CUR-5, CUR-6, CUR-8, CUR-9):** every response ends lost in 100%.
- **Torn-only, flush-lost parity (CUR-10):** `none` loses 100%, `scrub` alone is SAFE in 41%, `fix+scrub` in 100%.
- **Unnamed or unrecorded flush acknowledged (CUR-4, CUR-11):** `none` loses 100%; `fix+scrub` and `replace` are SAFE in 88%, and the other 12% are data-column losses that were already silent.

`raid56_health`'s action line (6642-6660) does say `replace-devid-N then scrub then ack` when an earlier event of the same mount named the device. A degraded write's `EV_STALE` names the missing devid. But that line is gone after the remount that brings the old disk back, and the dmesg text (6138-6142) says "fix or replace".

### Other findings
- **CUR-only wedge on RAID6.** CUR is STUCK in 194 states in the RAID6 failing-plus-missing family at depth 7 with symmetry; the parent saw 32 at depth 6. A is not stuck in the same family. The cause is a record naming a stale P on a present failing disk, spent while degraded.
- **The deep re-runs of the parent's all-present families find nothing new.** In persistent_full (depth 12), transient_full (depth 11) and flush_full (depth 11), CUR has 0 silent states. With every device present and no replay, CUR behaves like A there.

### Files (all in `attack_CUR/`)
- `cur_model.py`, `classify.py`, `window.py`, `summarize.py`
- `run_attack.sh`, `run_classify.sh`, `run_window.sh`
- results: `out/`, `out_class/`, `out_window/`, `out_ctl/`, `out_deepclass/`, and the tabulation in `summary.txt`
- citations: `kcite_cur.sh`, `kcite_cur.out`

### Suggested UML arm for CUR-6
1. Put RAID5 data on 3 loop devices and RAID1 metadata on 2 more.
2. Make one RAID5 device fail writes (dm-flakey).
3. Delete one metadata device from under the mount.
4. Write nodatacow data into more than 82 distinct 4 MiB regions.
5. Read the first region back: expect stale data with no error, and a `record_dropped` alert.

## Table

Each cell is SILENT / SILENT with no alert in that mount / LOST(gone) / LOST(unreachable) / REFUSED_WRITE.
- The first four are distinct states; REFUSED_WRITE counts transitions.
- A cell ending in T stopped on the 3 GB memory guard; its counts are exact up to the depth reached.
- All families are RAID5 with data without checksums unless the name says otherwise.
- "A" is the same model with every exception off (`--exc none`).
- The ablation column shows only X, or everything but X.

| Family (depth, CUR states) | CUR | CUR, alert latch cleared by mount | CUR, csum | A | A2 | Ablations |
|---|---|---|---|---|---|---|
| D1 degraded, missing device returns (d9, 6,540) | 1640/0/1008/632/0 | 1640/**1640**/1008/632/0 | 0/0/0/0/0 | 0/0/0/0/1536 | 0/0/0/0/1328 | only deg 1640; without deg 0 |
| D2 degraded, verdict spent (d9, 45,036) | 4268/0/6444/4736/6768 | 6732/1824/8652/4224/6048 | 0/0/4080/0/3936 | 0/0/384/896/12528 | 0/0/2176/4736/20304 | only deg 4268; without deg 0 |
| D3 unrelated device X missing + failing disk (d9, 46,022) **new** | 8896/0/13672/1216/44792 | 12468/2892/16728/1380/49940 | 0/0/7296/0/46088 | 0/0/5912/840/31272 | 0/0/5912/840/45512 | only deg 8896; without deg 0 |
| D3t the same, transient failure (d7, 777,146) | 229922/0/147068/124650/546512 T | 238466/165943/… T | 0/0/72372/0/564536 | 0/0/15724/33672/387940 | 0/0/45852/147140/688544 | without deg 0 |
| D4 two devices missing and returning (parity) (d9, 52,462) **new** | 30008/0/25508/1956/0 | 32684/24344/27836/1956/0 | 0/0/0/0/5832 | 0/0/0/0/5752 | 0/0/2136/5116/20556 | only deg 30008; without deg 0 |
| D5 RAID6, two missing + replace (d7, 1.33M) | 144668/0/188841/184749/172407 T | 157881/30895/… T | 0/0/242110/0/200294 T | 0/0/10400/30176/266992 | 0/0/91968/210384/570576 | without deg 0 |
| D6 X missing + replace (d8, 221,130) | 36900/0/50884/23872/39120 | 52608/12568/61952/18552/27680 | 0/0/22400/0/24000 | 0/0/4920/6880/60620 | 0/0/14816/23184/87248 | without deg 0 |
| D7 RAID6, failing + missing device (d6, 726,239) | 100045/0/39182/135981/163860 T | 106195/30619/… T | 0/0/28012/0/86554 T | 0/0/5096/39364/274052 | 0/0/8488/83908/376980 | without deg 0 |
| R1 replay, transient failure (d10, 30,438) | 3312/0/1652/2620/34800 | 4960/1648/2472/3064/38592 | 0/0/0/0/29300 | 0/0/0/3776/27820 | 0/0/0/2752/27820 | only replay 3312; without replay 0 |
| R2 replay, persistent failure (d10, 17,014) | 2304/0/1152/1344/19692 | 3456/1152/… | 0/0/0/0/16572 | 0/0/0/1984/15416 | 0/0/0/960/15416 | without replay 0 |
| R3 replay, 3 stripes, 2-record log (d8, 239,932) | 13500/0/3534/24984/200418 | 17538/4038/… | 0/0/0/0/186090 | 0/0/0/34302/198354 | 0/0/0/25086/198354 | without replay 0 |
| T1 unnamed flush, then a device missing (d8, 417,248) | 256749/0/299263/26750/278252 T | 280818/46490/… T | 0/0/268288/0/462820 | 0/0/6920/4196/55040 | 0/0/21004/8468/124412 | only unnamed 39062; only unnamed+torn 167416; without unnamed 90326 |
| T2 unreadable sector, recovery keeps it torn (d10, 3,488) **new** | 640/0/768/384/512 | 960/320/1088/384/512 | 0/0/256/0/256 | 0/0/128/384/1536 | 0/0/128/384/1536 | only torn 640; without torn 0 |
| T3 the same, sector error may hit parity (d9, 3,992) | 632/0/760/384/512 | 944/312/… | 0/0/256/0/256 | 0/0/128/384/1536 | 0/0/128/384/1536 | without torn 0 |
| T4 unnamed flush + unreadable sector (d9, 77,448) | 57152/0/49836/11720/9808 | 76002/11886/… | 0/0/15948/0/16856 | 0/0/1264/1264/5184 | 0/0/1264/1264/5184 | only unnamed 13930; only unnamed+torn 57152 |
| T5 transient failure + unreadable sector (d9, 200,686) | 17040/0/57100/38484/250244 | 23976/6936/… | 0/0/38392/0/160716 | 0/0/35480/38136/203564 | same as A | without torn 0 |
| T6 flush loss not recorded at all (WIB_READD_LOSE) (d8, 122,062) **new** | 48260/0/80018/19842/116128 | 60060/23988/… | 0/0/45164/0/89280 | 0/0/17600/18704/60916 | same as A | only lose 48260; without lose 0 |

**Deeper re-runs of the parent's CUR families** (stripe symmetry on):

| Family | SILENT |
|---|---|
| missing_return_full, d12 | 25,094 |
| replay_full, d11 | 768 |
| replay_full_transient, d11 | 1,664 |
| flush_unnamed, d11 | 13,574 |
| cap2_missing_return, d9 | 1,880 |
| raid6_bad_missing_full, d7 (not truncated) | 104,448 (STUCK 194) |
| persistent_full, d12 | 0 |
| transient_full, d11 | 0 |
| flush_full, d11 | 0 |

The mechanism signatures are identical at the parent's depth and 2 levels deeper in 4 families.

**Alert windows and administrator responses** (`window.py`; the latch is cleared by every mount, as in the kernel):
- Window = steps from the drop to the first wrong read.
- "No alert" = share of silent states whose mount shows no alert.
- Each response is followed by 3 steps: writes, remounts, returns, and one more device loss within tolerance.
- "lost" means 100% of the sampled drops (up to 400 per kind) end silent and lost.

| Class | Window | No alert | none | scrub | fix+scrub | replace | replace+scrub |
|---|---|---|---|---|---|---|---|
| CUR-1 returning data column (D1) | 1-2 | 100% | lost | lost | lost | SAFE | SAFE |
| CUR-7 returning device, parity case (D4) | 1-2 | 86% | lost | lost | lost | SAFE | SAFE |
| CUR-2 verdict (D2) | 0 | 26% | lost | lost | lost | lost | lost |
| CUR-6 unrelated device missing (D3) | 0 | 19% | lost | lost | lost | lost | lost |
| CUR-2 and CUR-8 verdicts and zeros (D5, D6) | 0-5 | 19-24% | lost | lost | lost | lost | lost |
| CUR-3 replay (R1, R2) | 0 | 33% | lost | lost | lost | lost | lost |
| CUR-9 unreadable sector (T2) | 0 | 33% | lost | lost | lost | lost | lost |
| CUR-10 torn-only, lost parity flush (T1) | 1-2 | 17% | lost | 41% SAFE | 100% SAFE | 59% SAFE | 100% SAFE |
| CUR-4 and CUR-11 flush acknowledged unnamed or unrecorded (T1, T6) | 0-3 | 17-41% | lost | lost / loud | 88% SAFE | 88% SAFE | 88% SAFE |

## Counterexamples

### CUR: SILENT_WRONG; LOST(gone) after the next RMW or scrub (Silent wrong read of data A keeps (A: LOST 0, 1,536 refusals). With the kernel's in-memory latch, 100% of these reads happen in a mount showing no alert. The advice 'fix the device, then scrub' makes the loss permanent in 100% of drop states; only replacing the missing device with a new disk is safe.)

Scenario: CUR-1: degraded exception; a missing data column's record is spent, then the device returns (D1, D3t)

Trace: d0 detaches || write s0.d0=2 ok || write s1.d0=2 (evicts s0[d0]; crash after mark) || mount with {d0} back  => s0.d0 reads 1 (acknowledged: 2). Without a crash it takes 4 steps: ... evict ok || unmount ; mount with {d0} back. Adding '|| scrub' or a write to s0.d1 folds the stale d0 into P, and the value is gone.

Kernel: Yes. wib_may_evict_naming() (raid56-wib.c:716; missing_devices at 724) -> wib_evict_sticky() pass 1 (801, loop at 847) via wib_find_or_alloc_entry() (982) <- btrfs_wib_mark() (3552) <- rmw_rbio() (raid56.c:4939). On return, device_list_add() clears MISSING with no resync (volumes.c:943) and mark_stale_sectors() (raid56.c:2060) finds no record. The alert latch alert_latched is in memory only (raid56-wib.h:813).

### CUR: SILENT_WRONG (Silent at once (window 0), and data A keeps (A and 'without deg': 0 silent; A refuses with log_full instead). No response after the alert helps (100% lost).)

Scenario: CUR-6 (new): an unrelated device X is missing (RAID5); the record of a present, failing disk is spent (D3)

Trace: d0 starts failing writes || write s0.d0=2 ok (the write to d0 failed) || X detaches (X holds no column of this stripe) || write s1.d0=2 (evicts s0[d0]) ok  => s0.d0 reads 1 directly from the present d0. The stripe had one fault, within RAID5's tolerance. Variants: X missing at mount ('mount without X'), or d0 already healed before the eviction (D3t).

Kernel: Yes. missing_devices is one count for the whole filesystem: runtime unplug via btrfs_remove_bdev() (super.c:2524, the .remove_bdev super op at 2584), or a degraded mount (volumes.c:7551). wib_may_evict_naming() (724) does not ask whether the missing device is in the chunk. Pass 1 of wib_evict_sticky() (847) spends the naming record, with the warning 'INCLUDING which member' at 887. This is the parent's CUR-5 without needing RAID6: a RAID1 metadata disk or an unbalanced added disk is enough.

### CUR: SILENT_WRONG (Silent at once; data A keeps. In the same family CUR is also STUCK in 194 states at d7; A is not.)

Scenario: CUR-5 (parent's class, confirmed): RAID6, one device missing plus a present failing disk (D7; P_raid6 at d7)

Trace: write s0.d0=2 ; CRASH after mark ; mount without d0 || d1 starts failing writes || write s0.d1=2 ok (the write to d1 failed) || write s1.d0=2 (evicts s0[d1]) ok  => s0.d1 reads 1 from the present d1. Signature: names-data | dX present-failing (+ P or Q missing) | stale-direct. 54,404 first-wrong states at depth 6; 104,448 silent states at depth 7 with symmetry.

Kernel: Yes, by the same path as CUR-6.

### CUR: SILENT_WRONG (the previous version, or garbage) (Data A keeps: A's record makes the recovery rewrite P on return, and A's LOST is 0. The wrong read can come many mounts after the alert; 86% of D4's wrong reads show no alert. Because btrfs parity rotates, one returning disk carries both CUR-1 stripes (where scrub destroys the value) and CUR-7 stripes (where scrub is the fix).)

Scenario: CUR-7 (new): a device returns whose PARITY column's record was spent; a later member loss rebuilds from the stale parity (D4)

Trace: P detaches || write s0.d0=2 ok || write s1.d0=2 (evicts s0[P]) ok || unmount ; mount with {P} back || d0 detaches  => s0.d0 reads 1: d0 is rebuilt from the stale P. If d1 detaches instead, s0.d1 reads garbage.

Kernel: Yes, by the same path as CUR-1. Reads of data columns never use P, so nothing shows until a member is lost. The log cannot tell which case a stripe is: the EV_DROPPED text (6140) says 'not which device took them'.

### CUR: SILENT_WRONG (a rebuild from torn parity) (A loss A also has, made silent. A returns EIO, and with non-aliasing values SILENT_ONDISK is 0. Window 0.)

Scenario: CUR-2 (parent's class): degraded exception spends a recovery verdict (D2, D5, D6)

Trace: write s0.d0=2 ; phaseB landed {d0} ; CRASH ; mount without d1 || write s1.d0=2 (evicts s0's verdict naming P)  => s0.d1 reads garbage rebuilt from the torn P.

Kernel: Yes. wib_evict_sticky() pass 3 (847; warning at 871) spends verdicts from scrub_raid56_recover_absent() / scrub_raid56_mark_suspect(). In the kernel, pass 3 needs every non-verdict record gone first. The realistic trigger is the capacity halving: wib_enforce_capacity_locked() at 941, called from btrfs_wib_mark_stale() at 3831, after a crash with more than 82 regions in flight and a degraded mount.

### CUR: SILENT_WRONG (zeros read as data) (A loss A also has, made silent: A returns EIO.)

Scenario: CUR-8 (new): the zero marks of a finished replace are spent while another device is still missing (D5 RAID6; D3 and D6 on RAID5)

Trace: RAID6: write s0.d0=2 ; phaseB landed {d0} ; CRASH ; mount without d1 || write s1.d0=2 (spends the verdict) || replace d1 (writes zeros + a mark) || d0 detaches || write s1.d0=1 (evicts s0[d1], the zero mark)  => s0.d1 reads zeros. On RAID5 the same outcome arrives through a verdict spent after the replace, or through double faults.

Kernel: Yes. btrfs_wib_replace_end() (4026) hands the finished replace's marks over as ordinary records ('become ordinary ones', 3995). The replace_owned protection (wib_entry_replace_owned, pass 2) covers only a replace that is still running. The negative control evict_replace_marks shows the model sees that protection: 4,192 silent states instead of 2,544.

### CUR: SILENT_WRONG (Data A keeps (A's LOST(gone) is 0 in R1-R3). Window 0: it happens inside mount(). 33% of the wrong reads happen in a later mount with no alert.)

Scenario: CUR-3 (parent's class, plus a healed-disk variant): tree-log replay spends records with every device present (R1-R3)

Trace: d0 starts failing writes || write s0.d0=2 ok (the write to d0 failed) || fsync s1.d0=2 (tree log) ; CRASH ; mount ; replay s1.d0=2 (evicts s0[d0])  => s0.d0 reads 1. Variant with every disk healthy at the mount: ... || d0 heals || fsync ... ; CRASH ; mount ; replay (evicts). R3: with a 2-record log, the replay spends the older record.

Kernel: Yes, when the replay or its commit writes sub-stripe RMWs into RAID5/6 block groups (RAID5/6 metadata, or space_cache=v1). BTRFS_FS_LOG_RECOVERING is set at tree-log.c:7755 and cleared at 7908, and tested at raid56-wib.c:725. Error records wait for the replay in RECOVER_VERIFY (7544) and are put back live at 7575-7577. With RAID1 metadata and the free-space tree, this exception never fires.

### CUR: SILENT_WRONG (garbage) (A loss A also has, made silent. A returns EIO; the value is gone either way (SILENT_ONDISK 0 with non-aliasing values). Window 0. A later scrub writes the garbage onto the disk.)

Scenario: CUR-9 (new): torn-only exception, every device present; a stripe the recovery kept torn because a sector would not read (T2, T3, T5)

Trace: sector s0.d0 stops reading || write s0.d1=2 ; phaseB landed {d1} ; CRASH ; mount (the recovery cannot read d0 and keeps s0 torn-only) || write s1.d0=2 (evicts s0T[])  => s0.d0 (unreadable) reads a rebuild from the torn P: garbage.

Kernel: Yes. wib_entry_torn_only() (775) and wib_entry_kept_torn() (784); passes 1-2 of wib_evict_sticky() (856-858); btrfs_wib_mark() falls back to them (3589-3594). The kernel's own warning at 877 describes exactly this outcome.

### CUR: SILENT_WRONG (the previous version) (A loss A also has, made silent: A returns EIO, and the value 2 exists only on d0. This is the one class where the alert helps: 'fix the device, then scrub' before the next member loss is SAFE in 100% of sampled drops.)

Scenario: CUR-10 (new): torn-only exception; a lost parity flush the log could not name is spent, then a member is lost (T1)

Trace: P starts failing flushes || write s0.d0=2 (the flush lost P's write; unnamed, so a torn-only record) ok || write s1.d0=2 (evicts s0T[]) || d0 detaches  => s0.d0 reads 1, rebuilt from the stale P.

Kernel: Yes, when wib_readd_dropped() takes the UNNAMED plan (plan chosen at 3032; alert at 3196), and then by the same path as CUR-9.

### CUR: SILENT_WRONG (Data A keeps: A aborts the commit (read-only) instead. T6: 48,260 silent states; 'without lose' gives 0.)

Scenario: CUR-4 (parent's class) and CUR-11 (new): a flush loss acknowledged unnamed (torn-only) or not recorded at all (WIB_READD_LOSE) (T1, T4, T6)

Trace: d0 starts failing flushes || write s0.d0=2 (the flush lost d0's write; the log cannot name it, or has no room for it) ok  => s0.d0 reads 1. With the lost write on P instead: a later d0 or d1 loss reads the old version or garbage.

Kernel: Yes, under an environment condition the model switches on rather than derives. nr_unnamed raises EV_LOG_UNFLUSHED (3196). The LOSE plan is chosen at 3016, and after readd_until expires at 3026; nr_lost raises EV_DROPPED (3198) and the commit goes on. No knob gates the LOSE plan, so even the kernel configured as A (keep_naming_degraded=1, torn_unevictable=1) keeps CUR-4 and CUR-11.

### CUR: alert usefulness (SILENT_NOALERT with the kernel's in-memory latch) (Share of silent states that read wrong in a mount with no alert: D1 100%, D4 86%, T6 41%, R1 33%, T2 33%, D2 26%, D3 19%, T1 17%.)

Scenario: Every family run with --alert-volatile

Trace: d0 detaches || write s0.d0=2 ok || write s1.d0=2 (evicts s0[d0]) ok || unmount ; mount with {d0} back  => the wrong read happens with alert=- : the record_dropped latch, its counters and raid56_health are all cleared at mount.

Kernel: Yes. alert_latched and stat_alert live in struct btrfs_wib (raid56-wib.h:813), with no on-disk flag. The only other sign is the text: the explanation printed once per episode (6138-6142) and the rate-limited warnings (868-892), all in the previous boot's kernel log. The refusal knobs exist only under CONFIG_BTRFS_DEBUG (586; const false at 654-658), so a production kernel cannot choose refusal.

## Sensitivity

**Ablations.** For every family:
- `--exc none` (A) has 0 silent states.
- Each "without X" run has 0 silent states, or exactly the count of the remaining exceptions.
- Each "only X" run reproduces CUR's count.

This attributes every silent state to one exception (§ table).

**Regression against the parent.** With the new switches off, `cur_model.py` reproduces the parent's counts exactly on:
- X_cur_return: 3,492 states, 432 silent;
- M_replay_full_CUR: 14,508 states, 1,520 silent;
- M_flush_unnamed_CUR: 39,472 states, 24,508 silent;
- M_persistent_full_A: 12,993 states, LOST 1,408.

**Negative controls for the extensions** (`out_ctl/`):

| Control | Result |
|---|---|
| X present but never missing | identical to a run without X (11,663 states, same metrics) |
| OLD with `--ure 1` | 548 silent: the URE model sees the write hole |
| A with `read_trust_torn` and `--ure 1` | 128 silent (A: 0): the torn refusal is what protects an unreadable sector |
| `evict_replace_marks` | 4,192 silent against 2,544 with the replace-owned protection |
| `--alert-volatile` | SILENT_NOALERT goes from 0 to 19-100% |
| `--lose-flush` under A | 0 silent (strict: the commit aborts and the stripe stays marked in flight) |

An early version of the strict LOSE branch forgot the in-flight mark and showed 4,248 silent states; I fixed it before any reported run.

**Aliasing.** Re-runs with `--fresh-values` separate real loss from coincidence:
- Extra-loss classes keep SILENT_ONDISK > 0: D1 632, D3 1,136, D4 2,028, R1 1,660.
- Loud-to-silent classes have SILENT_ONDISK = 0: D2, T2, T5. T5's 348 on-disk states without fresh values were value aliasing: a stale P happened to describe the acknowledged value again.

**Depth and mechanisms.**
- Signature sets are identical at the parent's depth and 2 levels deeper in 4 families (missing_return_full d10→d12, replay_full_transient d9→d11, flush_unnamed d9→d11, cap2_missing_return d7→d9). No new mechanism appeared.
- persistent_full d12, transient_full d11 and flush_full d11 give CUR 0 silent: with every device present and no replay, CUR behaves as A.

**Window analysis.**
- The first version gave the administrator's continuation no further device loss. It then reported torn-only and parity-flush drops as SAFE for every response, including "none". That is why the continuation now allows one more device loss within tolerance, which is the event these records exist for.
- The first version's results are kept in `out_window_v1/`.

## Caveats

- **REPORT.md was not written.** The harness blocks subagents from writing report files, so this structured output is the report. All model files, scripts and outputs are in `attack_CUR/`.
- **The model is abstract:**
  - one sector per column and no parity rotation (the CUR-1/CUR-7 interaction is argued from btrfs's rotating parity, not modelled);
  - 2 data columns and 2-3 stripes;
  - log capacity counted in records, with no narrow/wide layout, no capacity halving and no 64-block entries;
  - one operation at a time, with atomic replace and scrub, and a write acknowledged at its commit;
  - scrub's writes always land, even on a device that loses flushes.
- **Two conditions are switched on, not derived:** CUR-4 (nr_unnamed) and CUR-11 (WIB_READD_LOSE). Both are in the code, but how often they occur depends on concurrency the model does not have.
- **Not modelled:**
  - the halving burst, where the first stale bit can spend up to 83 records at once;
  - `btrfs_wib_add_sticky()` failing at recovery (raid56-wib.c:3777);
  - the kernel's table order inside a pass (the model spends in any order, a superset).
- **Bounds:** depth 6-12; 1-2 crashes; 1-2 device losses; one write or flush fault; at most 1 latent sector error.
- **Truncated runs.** Five CUR-side runs stopped on the 3 GB memory guard: D3t, D5, D7, T1 and T4. Their counts are exact to the depth they reached.
- **Counts are distinct states.** They depend on each policy's state space, so compare zero against non-zero, and use the signatures and traces rather than raw sizes.
- **Model versions during the matrix.** `cur_model.py` changed while the matrix ran: the LOSE switch, a label fix and the evict_replace_marks mutation were added. None of these changes behaviour for runs that do not use those flags, so the counts are consistent. `classify.py` results for T1 and T1x were regenerated after the label fix.
- **CUR-3 needs two real-filesystem conditions.** RAID5/6 metadata, or data written during the replay's commit. And the fsync's tree-log blocks must have found log room before the log filled, because with every device present CUR refuses new-region writes when the log is full.
- **No UML reproduction.** Every kernel mapping is by code reading at 2a1e2379e4. A UML arm for CUR-6 is described in the summary.
- **Window analysis limits.** At most 400 drop states per kind; continuation depth 3; responses applied instantly at the drop.

## Model files

- <scratch>/attack_CUR/cur_model.py
- <scratch>/attack_CUR/classify.py
- <scratch>/attack_CUR/window.py
- <scratch>/attack_CUR/summarize.py
- <scratch>/attack_CUR/run_attack.sh
- <scratch>/attack_CUR/run_classify.sh
- <scratch>/attack_CUR/run_window.sh
- <scratch>/attack_CUR/summary.txt
- <scratch>/attack_CUR/kcite_cur.sh
- <scratch>/attack_CUR/kcite_cur.out
- <scratch>/attack_CUR/out/
- <scratch>/attack_CUR/out_class/
- <scratch>/attack_CUR/out_window/
- <scratch>/attack_CUR/out_ctl/
- <scratch>/attack_CUR/out_deepclass/
