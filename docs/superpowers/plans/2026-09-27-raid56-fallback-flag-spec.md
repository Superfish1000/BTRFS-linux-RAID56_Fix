# RAID5/6 write-intent log: refusal policy switch (strict default, loud fallback)

Implementation spec. It replaces the three competing designs ("minimal", "operator", "risk").

- **Base.** The strict-wip head that merges lanes af (8f5d2d78aa), ar (9836b48a88) and al (a914dbe913) on top of am 61b9dce00a.
- **Citations.** Line numbers are from those trees (am, af, ar, al) and will shift after the merge. The function names are the anchors.
- **Verification.** Every claim marked *(verified)* was checked by reading that code for this spec. Nothing was built or run.

---

## 0. Decisions on one page

**Default.** Strict refusal stays exactly as it is today.

**The switch.** One supported switch per filesystem, on production kernels:

- mount option `raid56_write_intent_policy=strict|fallback`;
- a runtime file with the same name, `/sys/fs/btrfs/<fsid>/raid56_write_intent_policy`.

It is not persisted on disk; the evidence of what it gave up is.

**The contract.** The fallback may keep a write, a commit or a mount going where strict would refuse it, under three rules (section 1):

1. It never makes a read return data the kernel knows to be stale, unchecked or zeros at that moment.
2. It never writes what it knows is wrong.
3. Every give-up is latched in the same log block that first lacks the record, and announced by stripe before any read can go wrong because of it.

**What it covers.** Two classes, which are the wedges users actually hit:

- **C1.** Records that only name the stale copy of a device that is absent. This covers a degraded log that is full (FL-01), a degraded tree-log replay (FL-02, degraded half) and a degraded mount's recovery (FL-25, degraded half).
- **C2.** A failed cache flush whose names the log has no room to put on disk: FL-21, FL-22, FL-23, FL-24, and the naming half of FF-7. The names are kept in memory, where reads, RMW and scrub honour them, and repairs are queued. Only a crash or an unmount before those repairs land loses them. The part of the K5 case the log lost track of goes on with an alert that states the time window.

Everything else keeps refusing under the fallback, and its text says the fallback does not change it (section 4, section 6).

**Two new latched events, bits 23 and 24.** They are persisted in the latch word and carry their devids. They are printed at crit from the alert work, never with interrupts off. Each give-up is also listed with its logical address in the new file `/sys/fs/btrfs/<fsid>/raid56_given_up`.

**Strict precision fixes that land with the flag.** They give nothing up and remove false alarms under both policies (section 3.4):

- FF-2: count a lost block only where a name falls on it;
- FF-10: a flush that fails only on a device with no RAID5/6 member refuses nothing;
- FL-26: the -ENOSPC errno is reported as -EIO;
- an overflow of the memory-name table is no longer silent.

**Debug knobs.** Every `raid56_wf_*` knob keeps its exact behaviour and remains the stage-0 control arm. The flag never reads, implies or aliases a knob. It adds its own predicates at the same sites the knobs use, plus the alerts.

### How the three designs were judged

| Point | minimal | operator | risk | Decision and why |
|---|---|---|---|---|
| Option shape | bool `raid56_fallback`, plus sysfs | enum `raid56_log_policy=strict\|best-effort`, remount applied before sync, no sysfs | enum `raid56_write_intent_policy`, mount_opt bit, no sysfs | **Enum `raid56_write_intent_policy=strict\|fallback`, plus sysfs, plus remount applying it before sync.** The name groups with `raid56_write_intent`, and the value can later be extended. sysfs is the no-sync, no-lock way to unwedge a live filesystem. The early apply removes the footgun of a remount aborting the filesystem it was meant to save. *(verified: super.c:1544 `sync_filesystem()` runs before 1556 `btrfs_ctx_to_info()`)* |
| Storage | fs_info field | mount_opt bit | mount_opt bit | **fs_info field, plus a ctx field and a "given" flag.** A sysfs store must not read-modify-write `mount_opt`, which `btrfs_ctx_to_info()` overwrites wholesale. *(verified: super.c:1439)* |
| Degraded eviction (FL-01) | stage 0: "any device missing" | A2 drop test | A2 drop test plus a whole-disk fence (INCOMPAT) | **A2 drop test, no fence in v1.** Stage 0's rule spends the records of a present failing device while an unrelated device is missing (CUR-5/6). That reads stale data now, and breaks rule 1. The fence is open question Q1. |
| "Missing" | `missing_devices` | `bdev == NULL` | `wib->absent` set | **`!dev->bdev \|\| MISSING`.** A scan clears MISSING without reopening the device *(verified: volumes.c:959-962)*. Hot-unplug sets MISSING but keeps `bdev` *(verified: af super.c:2513-2551)*. The test must catch both. |
| Replay with every device present (FL-02, CUR-3) | covered | not covered | not covered | **Not covered.** It reads stale data now. The way through is to disconnect the failing disk and mount `-o degraded,…=fallback`. |
| Torn-only records (FL-03) | covered, kept_torn included | covered, no kept_torn | covered, no kept_torn | **Not covered.** Under the fallback a failed flush keeps its names in memory and sets no torn marks. So torn-only records come only from the recovery (kept_torn, which would turn EIO into a guess), from debug knobs, or from blocks older kernels wrote. COMPARISON §5 says never to spend them. *(verified: in af, `e->torn` is set only by knobs in `wib_readd_dropped`, by `btrfs_wib_mark_suspect_parity`, by `wib_keep_torn` and by old-block load)* |
| Verdicts and replace marks (FL-04) | covered | not covered | not covered | **Not covered.** Spending a verdict turns EIO into a guess. Spending a replace mark kills the only way out of the degraded state. |
| Commit cannot write the log (FL-15/16/20) | not covered | covered | covered (log_behind) | **Not covered in v1.** Every mark's commit fails on the same condition (FL-26), so RAID5/6 sub-stripe writes, RAID5/6 metadata included, fail anyway. The latch for FL-16 cannot be written. The gain is small. |
| Failed-flush naming (FL-21/22/24) | stage 0: torn and drop | memory names | memory names | **Memory names (af's `@refused_names`, generalised).** Reads keep rebuilding, so rule 1 holds; loss happens only at a crash or unmount before the repairs land. |
| Recovery that cannot keep a record (FL-25/MR1-4) | drop all, MR2 skipped | spill to memory, verdicts included | only through FL-01's predicate | **Only through the C1 predicate in v1.** Spilling verdicts to memory touches every verdict query. A strict fix (a temporary over-capacity record while scrubbing) is the better answer for MR2. This is open question Q2. |
| Events | 1 | 5 | 4 | **2**, keyed by what the admin must do. Missing-device drops mean "never reconnect, replace". Flush losses mean "wait for repairs or scrub, check the device". 7 of the 32 latch bits stay free. |
| Printing | at the raise site | ring plus work | ring plus work | **Ring plus alert work.** Give-ups happen under `wib->lock` with interrupts off *(verified: al `raid56_alert()` calls `raid56_alert_explain()` synchronously, 7843-7917)*. |
| Ack | as today | as today, ack clears the list | plain ack returns -EAGAIN for fallback bits | **As today** (`ack` or `ack <seq>`). The given-up list is never cleared by an ack. `names_in_memory` is a condition, not an event: no ack clears it. |

---

## 1. The contract (docs, alert texts and tests all refer to it)

Under `raid56_write_intent_policy=fallback`, wherever strict refuses a write, a transaction commit or a read-write mount to keep a record, the log may instead give the record up, but only under these rules:

- **R1. It never changes a read now.**
  - After a give-up, no read returns data the kernel knows at that moment to be stale, unchecked or zeros.
  - What it gives up is protection against a *later* event:
    - the absent disk coming back;
    - a crash or unmount before a queued repair lands;
    - for the K5 lost-track remainder only, data whose location the kernel never knew.
  - Every read refusal is unchanged (RD1-RD5, MR5-MR7).
- **R2. It never writes what it knows is wrong.**
  - RMW refusals are unchanged (P20: write_refused, stripe_undecidable).
  - So are scrub's declines (SC1-SC3) and every replace refusal (RE1-RE6, FF-9).
- **R3. Every give-up is loud before it can matter.**
  - It is raised synchronously at the give-up site. That sets the latch bit before the next log block is built.
  - The give-up and its latch therefore reach the disk in the same block *(verified: every block carries `raid56_latched_now()`, al:1635, and `raid56_alert()` sets `alert_latched` synchronously)*.
  - It is listed with its logical address and devid in the kernel log (crit) and in `raid56_given_up`.
  - It is announced by poll(), a uevent and FAN_FS_ERROR.
  - It holds `raid56_health` at `failing` until acknowledged, across reboots.

The filesystem can still refuse, and go read-only, under the fallback. Section 4 lists where, and each of those texts says so.

---

## 2. Interface

### 2.1 Mount option

- **Syntax.** `raid56_write_intent_policy=strict|fallback`.
  - Parsed with `fsparam_enum`, as `fatal_errors` is (super.c:234, 573-586).
  - Opt enum `Opt_raid56_write_intent_policy`, value table `btrfs_parameter_raid56_write_intent_policy[] = { {"strict", …STRICT}, {"fallback", …FALLBACK} }`.
  - An unknown value fails the mount with `btrfs_err(NULL, "unrecognized raid56_write_intent_policy value %s")` and -EINVAL.
- **Default.** strict. No on-disk state.
- **Accepted everywhere.** It is accepted on any btrfs: ro or rw, with or without RAID5/6, with or without the log (`noraid56_write_intent`). Where there is nothing to give up it has no effect, so one fstab line works everywhere.
- **The only way to reach the mount-time points.** Those are the recovery in `open_ctree()` → `btrfs_wib_rw_mount()` (disk-io.c:3746) and the tree-log replay. `btrfs_ctx_to_info()` runs at super.c:1904, before `open_ctree()`. The sysfs file only exists from disk-io.c:3690.
- **Older kernels.** A kernel without the option rejects it (unknown parameter). The docs warn that an fstab entry ties the machine to kernels that have it.

### 2.2 Runtime switch

`/sys/fs/btrfs/<fsid>/raid56_write_intent_policy`, mode 0644, added to `btrfs_attrs[]` next to `raid56_health` (sysfs.c:1709-1711).

- **Read.** `strict\n` or `fallback\n`.
- **Write.** `strict` or `fallback` (`sysfs_streq`). Anything else returns -EINVAL. The store also requires `capable(CAP_SYS_ADMIN)`, beyond the file mode.
- **What the store does.** It calls `btrfs_raid56_set_policy(fs_info, p, "sysfs")` (section 2.4) and nothing else.
  - No I/O, no mutex, no waiting: the rule at sysfs.c:242-250 is that a store must not hold up unmount.
- **On a filesystem that went read-only on an error** (`BTRFS_FS_ERROR`), the store returns -EROFS and logs this `btrfs_warn`:
  > "raid56_write_intent_policy: this mount went read-only on an error and cannot be switched; unmount it and mount it again with -o raid56_write_intent_policy=fallback -- the log on the devices still lists every stripe"
- **On a read-only mount without an error,** the store is accepted. It takes effect at the next `remount,rw`, whose recovery then runs under it.
- **Why a sysfs switch exists.**
  - It unwedges a live filesystem without a remount's `sync_filesystem()`.
  - It wakes writers already waiting in `btrfs_wib_mark()` at once.
  - It works from a monitoring script that reacted to a uevent.

### 2.3 Remount and bind-mount semantics

**Given flag.** `struct btrfs_fs_context` gains `u8 raid56_policy` and `bool raid56_policy_given`. Parsing sets both.

**`btrfs_ctx_to_info()`** copies the policy into fs_info in two cases:

- at the first mount, always (the default is strict);
- at a reconfigure, only if `raid56_policy_given` is set.

A remount that does not name the option therefore keeps the current value, including one set through sysfs. This also closes the race in which a sysfs change between `btrfs_init_fs_context()` and `btrfs_reconfigure()` would otherwise be reverted.

**Early apply.** In `btrfs_reconfigure()` (super.c:1525), after `btrfs_info_to_ctx(fs_info, &old_ctx)` and **before** `sync_filesystem(sb)`:

- if `!mount_reconfigure && ctx->raid56_policy_given && ctx->raid56_policy == FALLBACK`, call `btrfs_raid56_set_policy(fs_info, FALLBACK, "remount")`;
- if the remount then fails, the `restore:` path puts the old value back with `btrfs_raid56_set_policy(…, old, "remount failed")`;
- give-ups made during that sync stand, and each was announced;
- turning the fallback **off** is applied at the usual place, after the sync.

Why the early apply: without it, the remount's own writeback meets the full log under strict, and with RAID5/6 metadata it aborts the transaction in the middle of the very command meant to prevent that. The debug knob `raid56_wf_policy_after_sync` (super.c) restores the late apply; it is the negative control.

**Other paths.**

- **Bind-mount reconfigure** (`mount_reconfigure`): the policy is kept, as `mount_opt` is (super.c:1541-1542).
- **Abort.** `btrfs_remount_rw()` still refuses read-write after an error (super.c:1306-1310). The option cannot undo an abort. The remount logs the same warn line as the sysfs store.
- **ro to rw with the option.** `btrfs_remount_rw()` → `btrfs_wib_rw_mount()` (super.c:1334) runs the recovery under the new policy.

### 2.4 Storage and sampling

**Field.** `fs_info->raid56_policy` (u8, `enum btrfs_raid56_policy { BTRFS_RAID56_POLICY_STRICT = 0, BTRFS_RAID56_POLICY_FALLBACK = 1 }`). It is written with `WRITE_ONCE`.

- The reader is `static inline bool btrfs_raid56_fallback(const struct btrfs_fs_info *fs_info)`, which does a `READ_ONCE`.
- It is **not** a `BTRFS_MOUNT_*` bit.

**`btrfs_raid56_set_policy(fs_info, p, how)`**, in raid56-wib.c:

1. `WRITE_ONCE`s the field.
2. If `fs_info->wib`, calls `wake_up_all(&wib->wait)`. Writers blocked in `btrfs_wib_mark()`'s bounded waits re-evaluate `wib_can_mark()`, which re-samples the policy.
3. Kicks the alert work with announce, so poll() wakes and a uevent carrying `BTRFS_RAID56_POLICY=` goes out.
4. Logs the change (section 6, M7).

**Sampling.**

- `wib_policy_locked()` (am:809) also caches `wib->fallback = btrfs_raid56_fallback(fs_info)`.
- When the fallback is on, it also caches `wib->nr_absent`: the number of devices in `fs_devices->devices`, walked under RCU as `raid56_alert()` already does, for which `btrfs_wib_dev_absent()` holds.
- Counting (`wib_count_entries_locked`, `wib_write_frees_room`, `wib_room_source_locked`) and spending (`wib_evict_sticky`) use one sample per locked section, so they never disagree when the switch flips.
- Sites outside `wib->lock` (`wib_readd_dropped`, `btrfs_wib_commit`, `wib_unlogged_name`) sample `btrfs_raid56_fallback()` **once per decision**. The event they raise must match the sample.
- The setter takes no lock. A flip can land between two decisions, never inside one.

### 2.5 Visibility

- **`btrfs_show_options()`** prints `,raid56_write_intent_policy=fallback` when fallback is set, next to `,noraid56_write_intent` (super.c:1153). Strict is not shown, like other defaults.
- **`btrfs_emit_options()`** logs with `btrfs_warn`, not the INFO macros:
  - the M7-on line at **every** mount and remount that ends with fallback set, not only on a change, so that a fallback left in fstab shows in every boot's log;
  - the M7-off line at `btrfs_info` when a remount turns it off.
- **`raid56_health`** gains `policy strict|fallback` (section 5.5).
- **uevents.** Every raid56 uevent carries `BTRFS_RAID56_POLICY=strict|fallback`.

### 2.6 Things the switch deliberately does not do

- It does not undo a refusal that has already latched: `readd_refused`, `unlogged_refused`, `recovery_full`, or an aborted transaction. The way out is unmount, then mount with the option. Nothing is lost by that, because the log on the devices still lists every stripe.
- It does not change the debug knobs, and they do not change it.
- It has no sub-modes. The enum leaves room for them later.

---

## 3. Covered refusal points

Two mechanisms carry both classes:

- the **absent masks** and the A2 drop test (for C1);
- the **memory-name table** (for C2).

### 3.1 Mechanism A: absent masks and the A2 drop test

**Definition.** `static inline bool btrfs_wib_dev_absent(const struct btrfs_device *dev)`: `!dev->bdev || test_bit(BTRFS_DEV_STATE_MISSING, &dev->dev_state)`.

- True means that nothing reads that device's columns; they are rebuilt from the others.
- Both tests are needed:
  - a device scan clears MISSING while `bdev` stays NULL (volumes.c:959-962);
  - hot-unplug sets MISSING and keeps `bdev` (af super.c:2513-2551).

**New in-memory fields in `struct btrfs_wib_entry`.** They are not written to disk.

- `u64 absent;`: the bits of `@stale` whose data column was on an absent device when it was named.
- `u64 absent_par;`: the same for `@stale_par`.
- **Invariants:** `absent ⊆ stale` and `absent_par ⊆ stale_par`.
- They are cleared wherever the matching stale bit is cleared: `btrfs_wib_clear_stale`, `wib_set_stale_par`, eviction, `btrfs_wib_forget_range`, entry reuse (am:1131), and `wib_replace_forget`.
- A path that sets a stale bit without setting its absent bit leaves the bit "present". That is the safe default: it is never spendable by C1.

**Who sets them:**

1. **Data columns.** `raid56.c:rmw_update_stale_data()` calls `btrfs_wib_mark_stale(fs_info, logical, len, absent)`, a new `bool absent` parameter. Here `absent = btrfs_wib_dev_absent(rbio->bioc->stripes[stripe].dev)`. The other callers (raid56.c ~4866, the put-back in `rmw_repair_first`, and any in scrub.c) pass `false`.
2. **Parity.** `raid56.c:rmw_update_stale_parity()` calls `btrfs_wib_update_stale_parity(…, stale, absent)`, with absent computed from the parity column's device.
3. **Readd and unlogged naming.** `wib_name_region()` splits `names->absent` into `absent` (data) and `absent_par` (parity).
   - Today a single mask (am:2844, 2851) mixes bit j of the data column with bit j of the parity. That is ambiguous.
   - `wib_readd_dropped()` and `wib_unlogged_name()` copy them onto the entry together with the names they add.
4. **The mount's recovery.** `wib_readd_stale()` (ar:8515) and `wib_readd_record()` compute the masks for the names they put back from `@pending`. They use a helper shared with `wib_name_region()` that maps each stale bit through the chunk map and tests `btrfs_wib_dev_absent()`.
   - This runs in process context, outside `wib->lock`, before the lock is taken.
   - Without it, after a crash, a degraded mount's kept records would never be spendable.
5. **Replace marks, verdicts and scrub marks** set none.

**Predicate.** It is used only when `wib->fallback`:

```c
static bool wib_entry_absent_only(const struct btrfs_wib *wib, const struct btrfs_wib_entry *e)
{
	return wib->fallback && wib->nr_absent &&
	       (e->stale | e->stale_par) &&
	       !(e->stale & ~e->absent) && !(e->stale_par & ~e->absent_par) &&
	       !e->torn && !(e->suspect_par | e->prior_par) &&
	       !wib_entry_replace_kept(e, U64_MAX);
}
```

- The whole 64-block entry is checked, not one stripe (COMPARISON §5: 3,590 silent states otherwise).
- `wib_evictable()` keeps `!e->bitmap && !e->pin` in front of it.
- With `wib->nr_absent == 0` the masks are void. After a replace of the absent device completes, nothing is spendable by C1 any more, which is safe.

**Where it applies** (ar names):

- **`wib_evictable()`**: `… || wib_entry_absent_only(wib, e)`.
  - `wib_count_entries_locked()`, `wib_write_frees_room()` and `wib_room_source_locked()` use `wib_evictable()`. So an absent-only record counts as room at once, as stage 0 counted named records.
  - It is not gated on the `spend_torn` escalation.
- **`wib_evict_sticky()`**: when `!wib->may_evict_naming && wib->fallback`, a new pass right after pass 0 spends `wib_entry_absent_only()` entries in table order.
  - Nothing else is added.
  - The stage-0 passes and their order are unchanged for the knobs.
  - A spend in this pass:
    - raises `BTRFS_RAID56_EV_FALLBACK_MISSING` through `raid56_fallback_gave_up()` (section 5.2), not `EV_DROPPED`;
    - prints no `btrfs_warn_rl` at this site;
    - increments `stat_sticky_evicted`, `stat_stale_evicted` and the new `stat_fallback_spent`.
- **`wib_enforce_capacity_locked()`** goes through `wib_evict_sticky()`, so it inherits the new pass.

### 3.2 Mechanism B: the memory-name table (af `@refused_names`, generalised)

af's table `struct btrfs_wib_refused_name { bytenr, stale, stale_par }`, `BTRFS_WIB_REFUSED_SLOTS` = 165 + 165 = 330 regions (af h:562-569), becomes the table of names held only in memory, for both policies. Under strict it is filled only while a refusal stands, as today. The changes:

1. **Gate.** `wib_refused_any()` (af:3357) becomes `READ_ONCE(wib->nr_refused_names) != 0`.
   - Today it also requires `readd_refused || unlogged_refused`.
   - Under strict this is equivalent: names enter the table only on paths that then refuse *(verified: `wib_refused_name_locked()` callers at af:3402, 3792, 3864, 4137)*.
   - Also add `u64 absent, absent_par` to the table entry (from `names->absent*`) and a `u64 added_jiffies`.
2. **Every consumer honours it as it honours a record's names.** This is the checklist. A consumer that is missed turns the fallback into stage 0's silent behaviour (RMW or scrub folding the stale column into the parity).
   - `btrfs_wib_stale()` and `btrfs_wib_stripe_state()`: already, through `wib_refused_stale_locked()`.
   - `btrfs_wib_any_stale()`: already, through the gate.
   - **`btrfs_wib_stripe_error()`** (af:5745): must count a block with a memory name as an error block. Today it reads only `e->sticky` and `pe->sticky` *(verified)*. So a stripe whose record was not kept at all (LOSE) reads as NO_ERROR to scrub.c:2749, 3415 and 5589.
   - **`btrfs_wib_recorded()`** (af:5865): must return true for a range with memory names. Today it reads entries only *(verified)*. Scrub uses it to decide whether to commit before it scrubs a block group (scrub.c:3740, 4471).
   - `rbio_stripe_recorded()`: fine through `btrfs_wib_any_stale()` and `btrfs_wib_stripe_state()`, so RMW goes through phase A and rewrites the column with FUA before phase B.
   - The replace (scrub.c:2722 `btrfs_wib_stale`): fine.
   - `btrfs_wib_snapshot()` and the stale-stripes ioctl: report memory names, flagged as memory-only.
3. **Retirement.** A memory name leaves the table:
   - when `btrfs_wib_clear_stale(…, durable=true)` clears the same bits. This is the FUA write-back of phase A, a repair, or scrub;
   - when `btrfs_wib_clear_sticky()` covers its blocks (a full-stripe write or a verified scrub);
   - on `btrfs_wib_forget_range()` (block group removed);
   - when the same bits go onto a record: a readd that lands NAMED, or re-entry (item 5).
4. **Repairs.** When a name is added for a member on a present device, queue its repair (`wib_queue_repairs_region()`), as for record names.
   - Names on absent devices get no repair. They stay until re-entry, the replace or the unmount.
5. **Re-entry.** Before the block is built in `btrfs_wib_commit()`, and in `btrfs_wib_unmount()` / `btrfs_wib_remount_ro()` before the final persist:
   - move memory names onto records while the set, with them, stays within `BTRFS_WIB_MAX_ENTRIES` (82 wide) and the admission cap (`wib_admit_max`);
   - use the readd's NAMED plan: `sticky |= bits`, then `stale`/`stale_par`, `absent*`, and `hold`/`hold_par` where a write is in flight;
   - oldest first.
   - What does not fit at unmount is recorded nowhere from then on. Log crit U1 (section 5.3).
6. **Overflow** (more than 330 regions).
   - Under **both** policies: log once per mount, at crit: "raid56: %u more regions of names a failed flush left than can be held in memory (%u): reads of those members return what the device holds". Today `refused_names_overflow` is set silently at af:3344-3346 *(verified)*.
   - Under the fallback: the strict refusal stands. `readd_refused` or `unlogged_refused` is set and the strict event is raised with clause M3b. The fallback never goes on with a name it holds nowhere.
7. **Health condition.** `names_in_memory = nr_refused_names`, counted in regions, and `overflow = refused_names_overflow`. They are shown in `raid56_health` and keep the state `failing` while non-zero. No ack clears them.

### 3.3 Covered points

| id | Site (post-merge names) | Trigger | Strict (today) | Fallback | Event, cause |
|---|---|---|---|---|---|
| **FL-01** (= P01, degraded half of FF-13) | raid56-wib.c: `wib_policy_locked`, `wib_evictable`, `wib_evict_sticky`, `wib_count_entries_locked`, `wib_write_frees_room`, `wib_room_source_locked`, `btrfs_wib_mark` | The log is full while a device is absent, of records naming its stale copy: every degraded write into a stripe with a column there makes one (about 82 scattered 4 MiB regions, roughly 328 MiB) | New-region writes fail with EIO and log_full. A metadata write aborts the transaction: read-only | Records passing `wib_entry_absent_only()` count as room, and the new pass spends them after pass 0. The write goes on. Records naming a present device, verdicts, torn marks, replace marks, pinned and in-flight entries are never spent; if only those are left, log_full stands with text M1c | `fallback_missing_dropped`, cause `LOG_FULL_DEGRADED`, logical = `e->bytenr`, blocks = `hweight64(stale\|stale_par)`, devids = the absent devices |
| **FL-13 / FL-14** (room only) | raid56.c `rmw_rbio` → `btrfs_wib_try_add_failed`; scrub.c `scrub_replace_add_record` → `btrfs_wib_try_add_sticky` → `btrfs_wib_try_mark` | A full-stripe COW write a device did not take, or a replace record, in a full log | EIO, or the replace aborts | Room comes from the FL-01 predicate. If there is none, the same refusal as strict (not covered, section 4) | as FL-01 |
| **FL-02, degraded half** (RP1 with a device absent) | same predicate while `BTRFS_FS_LOG_RECOVERING`; no replay exception | The tree-log replay writes into a log full of records naming an absent device | The replay fails, and so does the mount | The replay spends absent-only records and completes. The fsync'd changes are kept. Reachable only with the mount option | `fallback_missing_dropped`, cause `REPLAY` |
| **FL-25, degraded half** (MR1/MR3/MR4 where the records to keep, or the room, are absent-only) | `wib_add_sticky()` (ar:4714) → `btrfs_wib_try_mark()`; `wib_readd_record()` / `wib_readd_stale()` compute absent masks | The mount's recovery must keep more than a wide block holds and the log has absent-only records | `recovery_full`, `recovery_log_full`: the first mount fails, a remount stays read-only | `btrfs_wib_try_mark()` spends absent-only records to make room. `recovery_full` is set only when nothing fallback-spendable is left; its text then says why the fallback did not help (M4b). Reachable through the mount option or `mount -o remount,rw,raid56_write_intent_policy=fallback`, or the sysfs file on the read-only mount before the remount | `fallback_missing_dropped`, cause `RECOVERY` |
| **FL-22 + FL-23** (= FF-1, FF-2, FF-4, P08) | `wib_readd_dropped()` (af:3620), refuse decision at af:~3935 | A failed flush (commit barrier, log flush, persist_now, the disable's last block, a detach counted as a failed flush) must take back more than a block holds with names (UNNAMED), more than it holds at all (LOSE), or a WAIT that ran past `BTRFS_WIB_READD_WAIT` (30 s) | `readd_refused`, `log_flush_unnamed`: every commit fails, read-only; no log block is written again (FL-23) | `refuse = (nr_unnamed \|\| nr_lost) && !readd_acks_unnamed && !(fallback && !refused_names_overflow)`. Under the fallback: UNNAMED and LOSE names go to the memory table (af's existing branches; no torn mark); repairs are queued for those on present devices; `readd_refused` is never set, so FL-23's gate never trips. `nr_lost` counts only blocks where a name falls (S-a). No `LOG_UNFLUSHED`, no `EV_DROPPED` with logical 0, no `btrfs_err_rl` "too full to say so" | `fallback_flush_unnamed`, raised with `raid56_alert_failed()` (devids), cause `READD`, counts: named on disk, named in memory, first logical |
| **FL-21** (= FF-3, P09) | `btrfs_wib_commit()` owed branch after `wib_commit_readd_settle()` (af:7101-7118) | The readd is still owed after the bounded settle: writes in flight hold its room | `readd_refused`, `log_flush_unnamed`: read-only | The settle **still runs**. Its gate stays `!readd_refused && !readd_acks_unnamed && !commit_refuses_owed`, never the policy. If the readd is still owed under the fallback: `wib_readd_refused_names()` puts the planned names in memory and `readd_refused` is not set. The commit goes on. The final `ret < 0 && !owed && !commit_keeps_previous` check is unchanged, so -ENOSPC while owed keeps the previous block, as the knob path does, and the cause text says so. The readd stays owed; it lands NAMED (names retire from memory) or, at `readd_until`, becomes LOSE (FL-22). Writers held off by the owed readd (FL-12) are admitted by then (≤ 30 s) | `fallback_flush_unnamed`, cause `COMMIT_OWED` (seconds held; `previous block kept` flag) |
| **FL-24** (= FF-5, FF-6, P10) | `wib_unlogged_name()` (ar, the `refuse = unknown \|\| (any && live + nr_new > BTRFS_WIB_MAX_ENTRIES)` line) | A failed flush after full stripes were written COW that the log cannot name: no room, or `@unlogged_unknown` (table of 165 regions full, written while the log was disabled, or more than 64 data stripes) | All or nothing: `unlogged_refused`, `full_stripe_flush_unnamed`: read-only | Under the fallback, not all or nothing. Name regions in table order while set, last block and regions named so far stay within `BTRFS_WIB_MAX_ENTRIES`. A region that does not fit puts its names in memory (its unlogged row is cleared as when named). Repairs are queued for both. If `unknown` (after S-b), it goes on and reports the lost-track window: seconds since the failed device's last confirmed flush, and `unlogged_lost` writes. `unlogged_refused` is never set, except on memory overflow. `full_stripe_unnamed`'s silent early return (the first test in `wib_unlogged_name()`) is never taken | `fallback_flush_unnamed`, causes `UNLOGGED` (named / in memory counts) and `LOST_TRACK` (window, count) |
| **FF-7, naming half** | `btrfs_wib_commit()` disable branch (af:7008-7024) reads `readd_refused` / `unlogged_refused` | A failed barrier or detach during the disable's last commit | The disable's commit fails: read-only | FL-22 and FL-24 do not refuse, so the disable completes. If the final block itself cannot be written, it still fails as strict (FL-20, not covered) | as FL-22/24, with the text clause "the log is being disabled: nothing will name these from now on" |
| **FF-8** (detach) | af `btrfs_wib_device_lost()` feeds FL-21/22/24 | A device detached while mounted | Refuses only through FF-1..6 | Covered through FL-21/22/24. Names on the detached (absent) device get no repair and stay in memory until re-entry or unmount | cause text "was detached while mounted; what its cache held is gone" |

### 3.4 Strict precision fixes landing with the flag

They apply to both policies and give nothing up. Each has a debug knob that restores the old behaviour for the control arm.

- **S-a (FF-2).** In `wib_readd_dropped()`, `nr_lost += hweight64(back)` (af LOSE branch) counts every lost block *(verified)*.
  - Count only blocks where a name falls: `(nstale | npar) & back`, or every lost block when `!identified || written_unknown`.
  - Lost blocks with no name are what a confirmed flush would have dropped.
  - Knob: `raid56_wf_readd_counts_nameless`.
- **S-b (FF-10).** In `wib_unlogged_name()`, `unknown` refuses (strict) or reports lost track (fallback) only if a device in `failed` holds a stripe of a RAID5/6 chunk.
  - Helper `wib_failed_holds_raid56(fs_info, failed)` walks the chunk maps under `read_lock(&fs_info->mapping_tree_lock)`, outside `wib->lock`, in the commit path.
  - A replace target (devid 0) is in no chunk map. When it fails a flush, the replace fails through FF-9 as today, and the filesystem stays writable under both policies.
  - Under the fallback, a flush failure that gives nothing up logs one warn line and raises no fallback event:
    > "raid56: devid %llu, which holds no member of a RAID5/6 stripe, did not confirm a cache flush; no protection was given up"
  - Knob: `raid56_wf_unlogged_refuses_nonmember`.
- **S-c (FL-26).** In `btrfs_wib_mark()`, after `wib_commit_wait()` fails, map -ENOSPC to -EIO before returning, so `rmw_rbio()` never hands ENOSPC ("No space left on device") to an application for a log refusal.
- **S-d.** The overflow crit line of section 3.2 item 6.

---

## 4. Not covered (the fallback refuses exactly as strict)

With the fallback on, each of these must behave byte for byte as strict. The UML "not-covered proofs" check this (section 9.4).

| id | Why it is not covered |
|---|---|
| FL-01 with a present device named (CUR-5/6, FF-13 present half) | Spending the record makes that device's stale data without checksums read back **now** (R1). Stage 0 also refused with every device present. The text (M1b) tells the admin to disconnect the failing device and mount `-o degraded,…=fallback`, after which C1 applies |
| FL-02 with every device present (CUR-3, RP1) | Same as above, and a later `btrfs replace` of the present source would copy the stale column onto the new disk. Ways out: `ro,rescue=nologreplay`, or disconnect the failing disk and mount degraded with the fallback (M2) |
| FL-03 torn-only records (P03, CUR-9/10) | Under the fallback a failed flush sets no torn marks (its names go to memory), so the torn-only records left are the recovery's kept_torn (spending one turns an EIO into a rebuild nothing checked, now) or were made by debug knobs or older kernels. COMPARISON §5: never spend torn records. A scrub with every device present retires them (the existing text) |
| FL-04 verdicts and replace marks (RE4, P13, CUR-2, CUR-8) | A verdict is all that stands between a read and a rebuild from a possibly torn parity (R1). A replace mark spent fails the replace, which is the only way out of the degraded state. Verdicts do not grow at runtime |
| FL-05 / FF-12 pinned record | A window of milliseconds to one commit. Spending it makes memory and disk disagree, which becomes a permanent EIO after a crash. Strict follow-up: count a pin as room that is coming (section 11) |
| FL-06 straddling half | Spending it leaves the stripe partly recorded, which the prescribed scrub then declines. Under C1 both halves become spendable anyway once the named half goes |
| FL-07 / FL-09 / P04 admission caps; FL-08 / P05 replace mark fit | Loosening them leads to a set no block describes (K3). The fallback makes room upstream, so the cap only turns concurrency above 82 regions into a wait. The replace refusal costs only a retry |
| FL-10 / P17 fail-fast; FL-11 bounded waits | Nothing is traded; this is timing only. The bounded waits are what prevent lockups |
| FL-12 / P18 owed-readd refusal of new-region writes | Stage 0 behaved the same, and the permissive knobs are defects. Under the fallback the owed readd resolves within `BTRFS_WIB_READD_WAIT` (FL-21/22), so these writers wait at most about 30 s |
| FL-13 / FL-14 without room | Going on would acknowledge an unnamed stale column, or serve a replace's zeros as data. Refusing loses nothing: the extent is not referenced yet, or the replace is retried |
| FL-15 / FL-16 / FL-20 / P06 / P07: the commit cannot write the log | Each mark's own commit fails on the same condition (FL-26), so every RAID5/6 sub-stripe write fails regardless (RAID5/6 metadata included), and the FL-16 latch cannot be written because the log is what fails. `commit_keeps_previous` must never be reused: its `commit_prepare` half reopens the write hole (FL-17). Follow-up in section 11 |
| FL-17, FL-18, FL-19 | FL-17 gives no availability and reopens the write hole. FL-18's permissive side is a BUG(). FL-19's knob refuses more |
| FL-26 / P19 a mark whose own log write fails | Going on without a durable record is the write hole itself. Only the errno changes (S-c) |
| FL-25 remainder: MR2 (a present-column error record in SCRUB mode), MR3 verdicts and kept torn, MR3(c) VERIFY records awaiting the replay | MR2: the debug knob's path scrubs without the record and regenerates parity from the stale column during the mount (R2); skipping the scrub leaves a known-stale column read as good (R1). Verdicts and kept torn: R1. Recommended strict fix for MR2: scrub with a temporary over-capacity record (165 table slots against 82 wide), and stop only if the stripe must stay recorded. Q2 asks whether verdicts should be spilled to memory under the fallback |
| MR5, MR6, MR7 | Each relaxation hands readers, or a regenerated parity, a guess (R1, R2) |
| FF-9 / RE5 / P12, RE1, RE2 / P14, RE3, RE6 | Replace refusals cost only the replace. Relaxing one serves zeros, junk or unconfirmed data (R2). RE1: strict is already the more available side |
| FF-11 / P16 (K1) | A correctness ordering over microseconds. Covering it turns a known name into a silent loss for no availability |
| SC1, SC2 / P15, SC3 | Relaxing them regenerates parity from a stale or undecided column (R2). They cost one more scrub pass |
| RD1-RD5 / P21, P20 | Read and RMW refusals (R1, R2). A best-guess salvage read, if ever wanted, belongs in a separate read-only rescue option |
| Memory-name overflow (more than 330 regions) | The fallback never acknowledges a name it holds nowhere. The strict refusal stands (M3b) |

---

## 5. Alerts and latch

### 5.1 Events

Two events are appended after af's `BTRFS_RAID56_EV_REPLACE_UNFLUSHED = 22`, so `BTRFS_RAID56_NR_EVENTS` becomes 25 and `static_assert(NR_EVENTS <= 32)` still holds with 7 bits free:

| bit | enum | name | meaning | what the admin does |
|---|---|---|---|---|
| 23 | `BTRFS_RAID56_EV_FALLBACK_MISSING` | `fallback_missing_dropped` | the fallback dropped records that only named the stale copy of an absent device | never reconnect that disk; replace it; scrub; ack |
| 24 | `BTRFS_RAID56_EV_FALLBACK_UNNAMED` | `fallback_flush_unnamed` | the fallback went on while a failed flush's names were held only in memory, or while the log had lost track of full-stripe writes | keep the filesystem mounted until `names_in_memory` is 0, or scrub; check or replace the device; restore files without checksums written in a lost-track window; ack |

Both events are in `BTRFS_RAID56_LATCHED_EVENTS`, so they set `alert_failing`, persist in the latch word and keep the state `failing` until acknowledged. Add them wherever an event must be listed:

- `raid56_event_names[]`;
- `raid56_alert_explain()`: a stub that only returns, since these are explained from the work (5.3);
- the health action branch;
- `tools/testing/btrfs/raid56_wib_dump.py` EVENTS, which must also gain af's `replace_target_unflushed`;
- `raid56_alert_listen.py` expectations.

The give-ups are **not** reported through `record_dropped`, `log_flush_unnamed`, `full_stripe_flush_unnamed`, `recovery_log_full` or `log_commit_failed`:

- the texts of those events say "every commit FAILS / read-only / not made writable";
- `record_dropped` also fires under strict for pass-0 vague records, so an admin could not tell room-making from a policy give-up.

The strict pass-0 `record_dropped` is unchanged.

### 5.2 The raise path

```c
static void raid56_fallback_gave_up(struct btrfs_fs_info *fs_info, enum btrfs_raid56_event ev,
				    enum raid56_giveup_cause cause, u64 logical, u32 blocks,
				    const struct btrfs_wib_flush_failed *failed /* NULL: the absent devices */,
				    u32 arg /* seconds, counts: per cause */);
```

It is callable under `wib->lock` with interrupts off. Lock order stays `wib->lock`, then `alert_lock`.

1. Unless `raid56_wf_fallback_silent`, it calls `raid56_alert()` with the event and the devices:
   - `failed` for flush causes;
   - every device for which `btrfs_wib_dev_absent()` holds, for `fallback_missing_dropped`.

   This latches the bit synchronously, which is what R3 relies on.
2. For `fallback_missing_dropped`, `raid56_latch_note_dev()` may displace a devid noted for a non-fallback event when both `latch_devs` slots are taken. Today the slots are first come, first served *(verified: al:7304-7322)*.
3. It appends to a ring of **64** entries in `struct btrfs_wib`, under `alert_lock`: `{ seq (= alert_latch_seq), jiffies, ev, cause, logical, blocks, devid, arg }`, plus `giveup_overwritten`. The ring is allocated with the log and freed at unmount. **It is never cleared by an ack.**
4. It increments `stat_fallback[cause]`.
5. It schedules the alert work at most 1 s ahead, never later than it is already due.
6. **It prints nothing.** `raid56_alert()` must skip `raid56_alert_explain()` for the two fallback events; the work explains them.

**Knob `raid56_wf_fallback_silent`** (CONFIG_BTRFS_DEBUG): the give-ups raise nothing and add no ring entry. It is the negative control that proves every fallback arm's loudness checks can fail.

### 5.3 Kernel log

All output below comes from `raid56_alert_work()`, in process context. The explanation and the per-occurrence lines are at **crit**:

- `btrfs_crit` has its own bucket in `_btrfs_printk` (messages.c `printk_limits[2]`);
- raid56-wib.c logs nothing else at crit, so a flood of other btrfs err or warn lines cannot swallow these;
- long texts go through a crit variant of al's `raid56_explain_err()`, which splits at 880 bytes.

**Explanation.** Once per episode per event: the first time `alert_seen` lacks the bit, printed before the first one-liner of that event.

**E23, `fallback_missing_dropped`** (`%s` variant: "", "to let the tree-log replay of this mount finish, ", "for the mount's recovery, "):

> raid56: FALLBACK (raid56_write_intent_policy=fallback): the write-intent log was full while devid %s is missing, so %sinstead of failing writes it DROPPED records that only said which of that device's copies are stale (first at full stripe %llu; each is listed as a 'raid56 FALLBACK' line and in /sys/fs/btrfs/%pU/raid56_given_up). Nothing reads back wrong while that disk stays out: its copy is rebuilt from the other devices. But nothing records any more where it is stale, so if it ever comes back -- reconnected, or found again at a reboot -- data without checksums on it can read back an OLD version with no error, and a scrub or a replace with it attached makes that permanent. Do NOT reconnect devid %s. Replace it with a new disk ('btrfs replace start <devid> <new device> <mountpoint>'), then run 'btrfs scrub start <mountpoint>' and acknowledge with 'echo ack <unacknowledged_seq> > /sys/fs/btrfs/%pU/raid56_health'. Until the replace, writes go on into stripes that have no redundancy left. To stop dropping records: 'echo strict > /sys/fs/btrfs/%pU/raid56_write_intent_policy'.

**E24, `fallback_flush_unnamed`.** The cause phrase is one of:

- "writes in flight held the room for %u s";
- "more of those stripes than a log block holds";
- "after full stripes were written copy-on-write";
- "was detached while mounted; what its cache held is gone".

The `[...]` part is printed only for the lost-track cause.

> raid56: FALLBACK (raid56_write_intent_policy=fallback): devid %llu (%s) did not confirm a cache flush (%s), so writes it acknowledged may never have reached its disk, and the write-intent log had no room to record on disk which of its copies are stale. Instead of failing every transaction commit and making the filesystem read-only, it went ON: %u stripes are named on disk and %u only IN MEMORY (first at full stripe %llu; each is listed as a 'raid56 FALLBACK' line and in /sys/fs/btrfs/%pU/raid56_given_up)[, and it had LOST TRACK of full-stripe writes in the %u s since devid %llu's last confirmed flush: nothing records where they may be lost]. While this mount lasts, reads rebuild the copies named in memory and repairs are queued for them; raid56_health's names_in_memory counts what is left. If the machine crashes, or the filesystem is unmounted, before that count reaches 0 -- and at once for writes the log lost track of -- data without checksums that devid %llu lost can read back an OLD version, or content never written to that file, with NO error. Do now: keep the filesystem mounted until names_in_memory reads 0, or run 'btrfs scrub start <mountpoint>'; check devid %llu ('btrfs device stats <mountpoint>': flush_io_errs) and replace it if it keeps failing ('btrfs replace start %llu <new device> <mountpoint>'); restore files without checksums written in a lost-track window from their source. Then acknowledge with 'echo ack <unacknowledged_seq> > /sys/fs/btrfs/%pU/raid56_health'. To stop going on: 'echo strict > /sys/fs/btrfs/%pU/raid56_write_intent_policy'.

**One-liners.** One per ring entry not yet printed:

- at most **10 per work run**, with runs at least 1 s apart, so at most 50 per 5 s, inside printk's per-level budget of 100 per 5 s;
- if more are waiting, one line: "raid56 FALLBACK: %u more give-ups not printed; see /sys/fs/btrfs/%pU/raid56_given_up".

Formats, all prefixed `raid56 FALLBACK [seq %llu]:`:

- `dropped the record of devid %llu's stale copy in %u blocks at full stripe %llu (log full, device missing%s)`, where the suffix is empty, ", tree-log replay" or ", mount recovery";
- `devid %llu's possibly lost writes named only in memory in %u blocks at full stripe %llu (%s)`;
- `went on with devid %llu's failed-flush records still waiting for room for %u s (%s)`, where the suffix is "the devices keep the previous log block" when the commit kept it;
- `lost track of %u full-stripe writes since devid %llu's last confirmed flush %u s ago`.

**Minute summary.** A separate crit line, in the run that prints the existing warn summary, only while fallback counters grew. It is not appended to al's warn summary: that format string is already about 823 characters, near printk's 1024-byte record limit.

> raid56: FALLBACK summary since the last one: %llu records naming a missing device dropped, %llu stripes named only in memory (%u regions still held), %llu flushes whose full-stripe writes the log lost track of

**Hourly reminder.** A crit line once an hour while a fallback bit is latched and unacknowledged:

> raid56: reminder: raid56_write_intent_policy=fallback gave up protection (%s) %u minutes ago and nobody has acknowledged it; see /sys/fs/btrfs/%pU/raid56_health

**U1, at unmount or remount-ro** when memory names are left after re-entry (section 3.2 item 5):

> raid56 FALLBACK: unmounting with %u regions whose stale copies were named only in memory: %u went into the final log block, %u are recorded nowhere from now on (first at full stripe %llu) -- from the next mount, data without checksums there can read back an OLD version with no error; check those files against a backup

### 5.4 `/sys/fs/btrfs/<fsid>/raid56_given_up` (new, 0444)

- One line per ring entry, oldest first: `<seq> <seconds_ago> <event> <cause> <logical> <blocks> <devid>`.
- Then `overwritten <n>` when entries were lost to the 64-entry ring.
- It fits one page. It is memory-only: gone at unmount.
- It is the reliable per-stripe list, because the kernel log is ratelimited.
- The docs give a shell loop that runs `btrfs inspect-internal logical-resolve -o` over each entry's range in 4 KiB steps.

### 5.5 `raid56_health` (`btrfs_raid56_health_show`, al:8136)

The new lines are appended after `action`. Parsers key on the first token (`hv()`, `sed`), so this is backward compatible.

```
policy strict|fallback
names_in_memory <regions> overflow <0|1>
given_up <total fallback give-ups this mount>
caution none | do-not-reconnect-devid-<N>[,<M>] | disconnect-devid-<N>[,<M>]
```

The two event counters appear automatically (the loop over NR_EVENTS), and so do the `unacknowledged` names.

**State.** It is `failing` while `names_in_memory > 0`, whether or not anything is acknowledged. Compute it in both `btrfs_raid56_health_show()` and `raid56_alert_work()`.

**Action line.** Composed inside the existing `failing` branch:

- While `names_in_memory > 0`, prefix `wait-for-repair-or-scrub then `. Place it after `unmount then mount-rw` when the filesystem is aborted, which cannot happen under the fallback's own give-ups.
- When `fallback_missing_dropped` is latched, the `replace-devid-N` list includes the latched devids of that event. The existing list comes from `alert_devs`.
- If one of those devids has a bdev and is not MISSING now, it has come back. The whole action becomes `unmount then disconnect-devid-N then mount-degraded then replace-devid-N then scrub then ack`, and `caution` reads `disconnect-devid-N`. Otherwise `caution` reads `do-not-reconnect-devid-N`.
- The `stuck` branch (recovery_log_full) appends `, else mount-rw-with-fallback then replace-devid-N then scrub then ack` only when hint M4a says the fallback would help.

### 5.6 uevent and fanotify

**uevent** (`raid56_alert_work()`, al:~8070):

- Every uevent adds `BTRFS_RAID56_POLICY=`.
- When `last_event` is a fallback event, it also adds `BTRFS_RAID56_LOGICAL=<logical>` and `BTRFS_RAID56_SEQ=<unacknowledged_seq>`, so a udev rule can log the addresses durably.
- Fallback raises kick the work within 1 s, instead of waiting behind the 60 s `BTRFS_RAID56_ALERT_PERIOD` timer, which `queue_delayed_work()` does not shorten. That gives at most one uevent per second.

**FAN_FS_ERROR.** Nothing raises it today unless a write fails with EIO (extent_io.c:565) or the filesystem shuts down (messages.c:179). Under the fallback neither happens.

- `raid56_alert_work()` calls `fserror_report_metadata(fs_info->sb, -EIO, GFP_KERNEL)` once per run in which a fallback counter grew *(verified: include/linux/fserror.h)*.
- fserror drops events before `SB_ACTIVE` *(verified: fs/fserror.c:88-91)*. So a report that finds the superblock inactive (recovery or replay give-ups during mount) stays owed; the work re-kicks every 1 s until the superblock is active, and stops at unmount.
- Latches restored at mount (5.7) also owe one report.

### 5.7 Persistence, restore, and when the alert cannot be persisted

**The invariant.** A give-up reaches the disk only with its latch, because the latch bit is set before the block without the record is built. Selftest ST-9 checks it.

**When no block lands after a give-up** (the log write fails on too many devices): the commit fails under both policies (FL-16 is not covered). The devices keep the older block, which still lists the record. Nothing was given up on disk, and the in-memory effects (memory names) are protecting reads.

**Log not enabled** (the K5 lost-track cause for writes made while the log was disabled): no block from this commit carries the latch. `btrfs_wib_commit()` must write it before returning success, with al's `wib_stamp_latch_locked()` or `wib_write_latch_locked()` under `commit_mutex`, instead of waiting for `wib_latch_work`. If that write fails, add to E24's text: "this alert could not be written to the log and will not survive a reboot: keep this message".

**Unmount.** al 59e986466e writes the latch at unmount. An event raised by the unmount's own commit (for example a failed final barrier) is persisted by that path.

**Restore at the next mount.** `raid56_latch_restore()` (al:7333) keeps its warn line for other events. For bits 23 and 24 it logs at **crit**:

> raid56: an earlier mount ran with raid56_write_intent_policy=fallback and gave up protection nobody acknowledged: %s. That mount's kernel log and raid56_given_up listed the stripes; this mount cannot. See /sys/fs/btrfs/%pU/raid56_health.

If bit 23 is set and a latched devid is present now (`!btrfs_wib_dev_absent()`), it adds:

> devid %llu, whose records of stale copies were dropped while it was missing, is attached now: data without checksums on it can read back an OLD version with no error wherever the array was written without it, and nothing records where. Unmount, disconnect it, mount with -o degraded and replace devid %llu with a new disk; a scrub or a replace with it attached copies its stale data.

**What is not persisted,** stated in the docs:

- the ring;
- the per-stripe list;
- the counts;
- the policy itself.

The latch bits and their devids are persisted. An older log-aware kernel that mounts read-write masks the unknown bits (al:7343) and its next block drops them.

### 5.8 Ack

Unchanged (al `btrfs_raid56_health_ack()`):

- `ack` clears every latched bit;
- `ack <unacknowledged_seq>` clears only if nothing new arrived, and otherwise returns -EAGAIN;
- the docs and every fallback text tell scripts to use `ack <seq>`, because give-ups can arrive continuously;
- an ack clears `alert_seen`, so the next give-up is explained again;
- an ack never touches the ring or `names_in_memory`.

---

## 6. Changes to strict-mode refusal messages

The flag and its cost are named only where the fallback would actually clear the refusal, and always after the ways out that lose nothing. Where it would not help, the text says so, so that nobody reaches for it in vain. Read, scrub and replace refusal texts never mention it.

**Hints.** The hints are filled at the refusal site under `wib->lock`, before the alert is raised. `raid56_alert_explain()` may run while the caller still holds `wib->lock`, so it cannot compute them itself. They are stored in `struct btrfs_wib_refusal_hint wib->hint`, copied under `alert_lock`:

```c
struct btrfs_wib_refusal_hint {
	u32 absent_only;	/* records the C1 predicate would spend (computed as if the fallback were on) */
	u32 present_named;	/* records naming a present device */
	u32 verdicts, kept_torn, torn, replace;
	bool names_overflow;	/* the memory-name table would overflow (M3b) */
	u64 absent_devids[2];
};
```

**M1, log_full (runtime, `raid56_alert_explain` LOG_FULL non-replay branch, al:~7618).**

Under strict, before "State: …":

- **M1a**, if `absent_only`:
  > Last resort, if writes must go on before that replace: 'echo fallback > /sys/fs/btrfs/%pU/raid56_write_intent_policy' (or -o raid56_write_intent_policy=fallback at the next mount) lets the log drop the %u records that only name missing devid %s instead of failing writes, each announced as a raid56 FALLBACK alert. The cost: that disk's copy becomes stale for good where they are dropped, with nothing recording where -- never reconnect it, only replace it -- and writes go on into stripes with no redundancy left.
- **M1b**, otherwise:
  > raid56_write_intent_policy=fallback does not change this: it never drops a record naming a device that is present, the mount's verdicts, a possibly torn mark or a running replace's marks.

  If `present_named`, it adds:
  > If the device that keeps failing writes cannot be replaced now, disconnect it and mount with -o degraded,raid56_write_intent_policy=fallback: its records then name a missing device, which the fallback may drop; never reconnect that disk afterwards.

Under the fallback, when log_full is still raised, **M1c** is M1b's first sentence as the clause "(raid56_write_intent_policy=fallback is on but does not change this: …)", followed by M1b's second sentence when it applies.

The stage-0 text selected under `evict_stage0` stays as it is (knob only).

**M2, replay log_full (al d54548e1e6 branch).** Before "State:":

- if `absent_only`:
  > Or mount with -o degraded,raid56_write_intent_policy=fallback: the replay then drops the records that only name the missing device, each announced, and keeps the fsync'd changes that 'btrfs rescue zero-log' would give up; replace that device afterwards and never reconnect it.
- if `present_named`:
  > With the device that failed writes disconnected, 'mount -o degraded,raid56_write_intent_policy=fallback <device> <mountpoint>' lets the replay finish and keeps the fsync'd changes that 'btrfs rescue zero-log' would give up: the records naming the disconnected device are dropped, each announced. Never reconnect that disk; replace it.

**M3, `log_flush_unnamed` and `full_stripe_flush_unnamed` (the strict raid56_explain_err branches).**

- **M3a**, if `!names_overflow`, after the existing remedy:
  > This mount cannot be switched: it has gone read-only on an error. If the device keeps failing and cannot be replaced yet, mount with -o raid56_write_intent_policy=fallback after unmounting: commits then go on, the names the log has no room for are kept in memory (reads and repairs use them) and each give-up is announced. The cost: a crash or an unmount before those repairs land lets data without checksums that the device lost read back old with no error.
- **M3b**, if `names_overflow` (under either policy):
  > (raid56_write_intent_policy=fallback does not change this: it holds such names in memory only while they fit, %u regions, and there are more.)

**M4, `recovery_log_full` (al 0c6dbe4eed text, `raid56_rlf_advice`).** The hint is filled in `wib_add_sticky()` when `recovery_full` is set.

- **M4a**, if `absent_only`, before "Then acknowledge":
  > Or mount with -o %sraid56_write_intent_policy=fallback (from the read-only mount: 'mount -o remount,rw,raid56_write_intent_policy=fallback <mountpoint>'): it drops the records that only name missing devid %s, each announced, and the mount becomes writable if that makes room for the rest. The cost: that disk becomes stale for good where they were dropped -- replace it, never reconnect it.

  The `%s` is "degraded," when a device is missing.
- **M4b**, otherwise:
  > raid56_write_intent_policy=fallback does not change this: the records the recovery must keep are its verdicts on stripes it could not decide, or name a device that is present.

  M4b is also used under the fallback when `recovery_full` is still set.

**M5, `log_commit_failed` and `log_write_failed` under the fallback.** Append:

> (raid56_write_intent_policy=fallback is on but does not change this: the log itself cannot be written.)

For `log_write_failed` caused by an owed readd, keep today's text. Under the fallback it can only be the ≤ 30 s window.

**M6, mount failure lines.** At disk-io.c:3749 (and 3800, after the replay) and super.c:1336, when `wib->recovery_full` is set, replace "failed to replay raid56 write-intent log: -ENOSPC" with:

> failed to recover the raid56 write-intent log: it has more records to keep than a log block holds (recovery_log_full above)

**M7, policy lines.** The prefix is "set through sysfs: " or "remount: " as applicable.

- **On** (`btrfs_warn`):
  > raid56_write_intent_policy=fallback: where the RAID5/6 write-intent log would refuse a write, a commit or a read-write mount to keep its protection, it now gives up the records that only name a missing device's stale copy, and keeps in memory the names a failed cache flush leaves that it has no room for -- announcing every give-up as a raid56 FALLBACK alert. It never drops a record naming a present device and never changes a read the log refuses. Use it to keep a failing array running until it is repaired, then set it back: 'echo strict > /sys/fs/btrfs/%pU/raid56_write_intent_policy'.

  If a device is absent now, it adds: " devid %llu is missing: records naming its copy will be dropped when the log is full; never reconnect that disk, replace it."
- **Off** (`btrfs_info`):
  > raid56_write_intent_policy=strict: the write-intent log refuses again instead of giving up protection; what the fallback gave up stays listed in raid56_health and raid56_given_up until acknowledged.

  If `names_in_memory > 0`, it adds: " %u regions of names are still held only in memory until their repairs land."

**M8.** No other strict text changes. Read, scrub and replace refusals, stripe_undecidable and write_refused never mention the flag.

---

## 7. Debug knobs

- **Unchanged.** Every existing `raid56_wf_*` knob and the older `raid56_*` knobs keep their exact behaviour and remain the stage-0 control arms: evict_stage0, keep_naming_degraded, evict_naming, torn_unevictable, torn_spent_eagerly, kept_torn_in_order, evict_replace_marks, replace_keeps_added_only, admit_narrow, admit_recorded_free, readd_acks_unnamed, commit_keeps_previous, commit_refuses_owed, full_stripe_unnamed, recover_drops_records, refusal_leaves_unnamed, and so on. On a non-DEBUG kernel they are constant false and the flag alone decides.
- **Combining a knob with the flag.**
  - Union of permissions: the knob's broader predicate applies.
  - Any give-up at a covered site raises the fallback event while the flag is on.
  - In a DEBUG build, `btrfs_raid56_set_policy()` warns once if the flag is turned on while any permissive `raid56_wf_*` knob is set.
  - The fallback UML suites fail if any knob is not at its default.
- **New knobs**, all CONFIG_BTRFS_DEBUG negative controls:
  - `raid56_wf_fallback_silent` (5.2);
  - `raid56_wf_policy_after_sync` (2.3, in super.c);
  - `raid56_wf_readd_counts_nameless` (S-a);
  - `raid56_wf_unlogged_refuses_nonmember` (S-b).
- **Existing knobs reused as controls for the fallback:**
  - `raid56_wf_evict_stage0=1` with the fallback: stage-0 spending returns (CUR-5/6). This is the control for the `unrelated` arm.
  - `raid56_wf_refusal_leaves_unnamed=1` with the fallback: memory names are not kept. This is the control for the C2 read-back oracle.
- **`MODULE_PARM_DESC` additions** for evict_stage0, readd_acks_unnamed, full_stripe_unnamed, recover_drops_records and commit_keeps_previous:
  > the supported per-filesystem form is -o raid56_write_intent_policy=fallback, which does not <spend present-device records / verdicts / torn marks | mark unnamed stripes torn | skip naming | drop recovery verdicts | keep the previous block>

---

## 8. Documentation

1. **`Documentation/filesystems/btrfs-raid56-write-intent.rst`.**
   - In "Feature flag and enabling" (al:144), mention the option next to `noraid56_write_intent`.
   - Reword "Cost" ("a RMW that finds it full waits"): what strict does, what the fallback does.
   - **New section "Refusal policy: strict and fallback"**, placed after "Feature flag and enabling". It covers:
     - the default and why: nothing is ever returned wrong without an error;
     - the option and the sysfs file:
       - why sysfs exists (remount syncs first);
       - that the mount option is the only way to reach the mount-time recovery and the replay;
       - per mount, not on disk;
       - the remount rules;
       - the fstab caveat (older kernels reject it);
     - the contract (R1-R3);
     - a table of the two covered classes: trigger, what strict does, what the fallback does, what can go wrong later, what to do;
     - the list of what it never gives up (section 4, grouped), including that the filesystem can still go read-only;
     - the risk scope:
       - data with checksums and all metadata are still verified;
       - the exposure is data without checksums (nodatasum, nodatacow);
       - the model classes re-enabled, loudly: CUR-1/7 without the taint (disk returns), CUR-4/11 bounded to crash or unmount before repairs, K5 lost-track;
       - those never re-enabled: CUR-2, 3, 5, 6, 8, 9, 10, K1-K4;
     - recommended use: per incident, not fstab; turn it off after replace + scrub + ack.
   - **New section "raid56_health and the alerts".** Today the rst documents only statistics. It covers:
     - every key, including `policy`, `names_in_memory`, `given_up` and `caution`;
     - every event name with one line, marking the latched ones;
     - `ack` and `ack <seq>`;
     - the uevent variables;
     - FAN_FS_ERROR;
     - the `raid56_given_up` format and a shell loop that resolves it to files;
     - what survives a reboot and what does not.
   - **New section "Runbook"** with these scenarios:
     - degraded, no spare disk;
     - a flaky device fails flushes;
     - a device keeps failing writes (the fallback does not help: disconnect it and mount degraded);
     - recovery_log_full at mount;
     - a replay that fails at mount;
     - after a fallback give-up;
     - a dropped disk comes back.
2. **btrfs(5) in btrfs-progs** `Documentation/ch-mount-options.rst` (out of tree): the option entry, with the one-line meaning and a pointer.
3. **`tools/testing/btrfs/uml/README.md`:**
   - a `fallback` column in the scenario tables (fixed / control / fallback / fallback-control);
   - the new drivers;
   - the rule that fallback arms run with every knob at its default, and the fault-injection-free ones also on a non-DEBUG kernel;
   - the knob table rows for the four new knobs.
4. **`tools/testing/btrfs/models/policy/COMPARISON.md`:** a note that the fallback is "A2's drop test without the taint, plus memory names for flush losses, with a persisted latch". List which CUR classes it re-enables and which it never does.
5. **In-code comments** at each site in section 3: which class it accepts and why it differs from the knob next to it. This matters most in `wib_evict_sticky`, `wib_readd_dropped`, `btrfs_wib_commit` (settle kept) and `wib_unlogged_name` (partial naming).
6. **`raid56_wib_dump.py`**: the EVENTS list (af's event 22 plus 23 and 24).

---

## 9. Tests

### 9.1 Selftests (`fs/btrfs/tests/raid56-wib-tests.c`)

The dummy `fs_info` (tests:5688) sets `fs_info->raid56_policy`. Every eviction, readd, unlogged, commit and recovery test runs twice in the same run, once with the policy strict and once with fallback. With the policy strict every existing expectation must hold unchanged.

- **ST-1, `wib_entry_absent_only`:** spendable only with fallback and `nr_absent > 0`. Refused for:
  - one present bit (data or parity);
  - torn;
  - kept_torn;
  - suspect_par or prior_par;
  - replace-kept;
  - bitmap;
  - pin.
- **ST-2, absent masks:**
  - set by `btrfs_wib_mark_stale(…, true)` and `btrfs_wib_update_stale_parity(…, true)`;
  - `wib_name_region` splits data and parity;
  - the masks are cleared with the stale bits;
  - the invariants `absent ⊆ stale` and `absent_par ⊆ stale_par` hold after every operation (checked by a helper);
  - recovery re-add computes them.
- **ST-3, eviction order:**
  - pass 0 vague records still raise `DROPPED`, not FALLBACK;
  - absent-only records raise `FALLBACK_MISSING`, not DROPPED;
  - nothing else is spent under the fallback: a present-named, verdict, torn or replace log still fails the write with log_full;
  - the admission cap stays 82.
- **ST-4, readd under fallback:**
  - UNNAMED and LOSE put names in memory and set no torn bit;
  - `readd_refused` stays clear;
  - `FALLBACK_UNNAMED` is raised with the devid, and `LOG_UNFLUSHED` is not;
  - repairs are queued for present-device names only;
  - under strict it is identical to af.
- **ST-5, commit owed:**
  - the settle runs under the fallback (`nr_marking` drains first);
  - still owed: the commit returns 0, names are in memory, the event is raised;
  - strict refuses;
  - -ENOSPC while owed keeps the previous block.
- **ST-6, unlogged:**
  - partial naming names exactly the regions that fit `BTRFS_WIB_MAX_ENTRIES`, and the rest are in memory;
  - `unknown` goes on with the LOST_TRACK cause;
  - the S-b non-member case refuses nothing under strict.
- **ST-7, memory names:**
  - lookup through `btrfs_wib_stale`, `btrfs_wib_stripe_state`, **`btrfs_wib_stripe_error`** and **`btrfs_wib_recorded`**;
  - retirement by `clear_stale(durable)`, `clear_sticky` and `forget_range`, and when the readd lands;
  - re-entry at commit and at unmount (fit and overflow counts);
  - overflow sets the refusal under the fallback and logs crit under both policies.
- **ST-8, S-a:** LOSE whose lost blocks carry no name does not refuse under strict. With the knob it refuses.
- **ST-9, latch invariant:** after a fallback give-up, the next `btrfs_wib_build_block()` carries bit 23 or 24 in the latch word, with the devid in a latch slot, displacing a non-fallback devid when full. `latch_restore` round-trips them.
- **ST-10, sampling:** flip the policy between `wib_count_entries_locked` and a later section; one locked section never mixes samples. The event raised always matches the sample.
- **ST-11, errno:** a mark whose block cannot be built returns -EIO.

### 9.2 UML: per covered point

Every covered point gets these arms:

- **strict:** the existing fixed arm, unchanged.
- **fallback:** `OPTS=rw,raid56_write_intent_policy=fallback`, every knob at its default. It asserts all of these:
  - (a) the operation strict refused succeeds, and the filesystem is not read-only;
  - (b) `raid56_health` shows `policy fallback`, the event counter > 0, the event in `unacknowledged`, state `failing`, and `names_in_memory` as expected;
  - (c) `raid56_alert_listen.py` gets a uevent with `BTRFS_RAID56_EVENT=fallback_*` and `BTRFS_RAID56_POLICY=fallback`, a poll() wake, and a FAN_FS_ERROR, including for mount-time give-ups;
  - (d) dmesg holds the crit explanation exactly once per episode, plus a `raid56 FALLBACK [seq` line carrying the logical address;
  - (e) `raid56_given_up` lists that logical address;
  - (f) no strict event for the covered trigger;
  - (g) the data oracle: every acknowledged nodatasum block reads back as written during the mount;
  - (h) no KERNEL_SPLAT and no WATCHDOG.
- **fallback-control:** the same with `raid56_wf_fallback_silent=1`. The loudness assertions (b)-(e) must FAIL.
- **latch:**
  - unmount, then mount **without** the option: the event is still unacknowledged, with its devid, and the crit restore line is there;
  - `ack <stale seq>` returns EAGAIN, `ack <seq>` clears it;
  - `names_in_memory` is not cleared by the ack.

Per driver:

- **`degraded_log_full.sh`** (ar):
  - `column` + fallback: all 120 regions are written and metadata commits; `fallback_missing_dropped` with the removed devid; `caution do-not-reconnect-devid-N`; the platters of present devices are unchanged for dropped stripes.
  - **`unrelated` + fallback (key negative arm):** the new writes still fail with log_full, and the text contains M1c and M1b's disconnect advice. Control: fallback + `raid56_wf_evict_stage0=1`, where the oracle catches stale reads. This proves the A2 rule rather than `missing_devices`.
  - **`return` (new):** after the column/fallback run, unmount, reattach the dropped disk and mount strict. Check the crit restore line naming the devid, the action starting with `unmount then disconnect-devid-N`, and `caution disconnect-devid-N`. It asserts the alert, not the reads.
  - **`scan` (new):** a device scan of the returning disk while mounted (volumes.c:959) must not change what is spendable.
  - **`hotunplug` (new, af null_blk kernel):** remove a device while mounted. Its records become spendable (MISSING with a bdev).
- **`replay_full.sh`** (al):
  - every device present + fallback: the mount still fails with M2's disconnect text;
  - **`degraded` plan (new):** the failing device disconnected, `-o degraded,raid56_write_intent_policy=fallback`. The replay completes, the fsync'd file is present, and `fallback_missing_dropped` (REPLAY) is raised.
- **`commit_full.sh`** (al):
  - degraded-mount phase + fallback: kept records naming only the absent device make the mount read-write through `fallback_missing_dropped` (RECOVERY);
  - verdict plan: still `recovery_log_full`, the mount stays read-only, text M4b;
  - `remount,rw,raid56_write_intent_policy=fallback` from the read-only mount succeeds when M4a said it would;
  - the logio phase: still `log_commit_failed` + M5 under the fallback.
- **`flush_wedge.sh`:**
  - `unnamed` plan (CUR-4 setup) + fallback: the commit goes on and the filesystem stays read-write; `fallback_flush_unnamed` (READD) with the devid; `names_in_memory > 0`; every overwritten block reads back as written during the mount (rebuilt); after the repairs `names_in_memory 0`. Control: + `raid56_wf_refusal_leaves_unnamed=1`, where the oracle catches stale reads.
  - **unmount-before-repair arm:** the names go into the final block (U1 shows 0 recorded nowhere), and the next mount's recovery repairs them; the reads are correct.
  - **crash-before-repair arm:** the documented loss for names not in any block; the restore line is present.
  - `named` plan + fallback: a present device's names still refuse (log_full), as strict.
- **`readd_flush.sh`, `torn_readd.sh`, af `detach_flush.sh`, af `refusal_marks.sh`:** fallback arms with the assertions above. For detach, names stay in memory (absent device, no repair) until unmount.
- **`fullstripe_flush.sh`** (ar):
  - `refuse` plan (90 regions) + fallback: the regions that fit are named on disk and the rest are in memory. Assert the counts in the one-liners and in `names_in_memory`; the reads are correct during the mount.
  - `unknown` plan (more than 165 regions): the LOST_TRACK cause with the window; the filesystem stays read-write.
  - **`nonmember` plan (new, S-b):** a flush failure only on a replace target on a busy log. Under **both** policies there is no read-only switch and no fallback event, and the replace fails with `replace_target_unflushed`. Control: `raid56_wf_unlogged_refuses_nonmember=1`.
- **`readd_admit.sh` / a new `readd_nameless` plan (S-a):** a LOSE with no name on lost blocks does not refuse under strict. Control: `raid56_wf_readd_counts_nameless=1`.
- **Memory-name overflow (new plan in `flush_wedge.sh`):** a device that fails flushes at every commit until more than 330 regions are held. The strict refusal returns under the fallback, with text M3b and the overflow crit line.

### 9.3 New drivers

- **`policy_switch.sh`:**
  - Parsing, show and emit:
    - `-o raid56_write_intent_policy=fallback`: /proc/mounts shows it, `raid56_health` shows `policy fallback`, the M7 warn line is logged, and a uevent carries POLICY;
    - `=foo` fails the mount with the message;
    - accepted on a non-RAID56 filesystem, ro and rw.
  - Remount:
    - a remount that does not name the option keeps it;
    - `=strict` turns it off (info line);
    - a bind mount keeps it;
    - a failed remount restores the old value;
    - ro, then remount rw, keeps it.
  - sysfs:
    - read and write `strict` and `fallback`;
    - `foo` returns EINVAL;
    - a sysfs write followed by a remount without the option keeps the sysfs value;
    - on an aborted filesystem it returns EROFS with the warn line.
  - **`unwedge`:** RAID5 data + RAID1 metadata, degraded, strict, writes failing with log_full. `echo fallback` with no remount makes the next writes succeed. A writer blocked in the bounded wait completes within 2 s (the `wake_up_all`). `echo strict` makes the refusals resume.
  - **`remount-meta`:** RAID5 data and metadata, wedged, degraded. `mount -o remount,raid56_write_intent_policy=fallback` produces no "forced readonly", and later writes succeed. Control: `raid56_wf_policy_after_sync=1` aborts the filesystem.
  - **`aborted`:** strict refuses a commit and the filesystem goes read-only. A remount with the option is refused for rw, with the text. Unmount, then mount with the option, then repeat the fault: the filesystem stays writable and `fallback_flush_unnamed` is raised.
- **`fallback_flood.sh`:** degraded fallback, 5,000 region writes. It checks:
  - no WATCHDOG and no KERNEL_SPLAT;
  - no more than 10 crit one-liners per second, plus "more … not printed" lines;
  - the ring ends with `overwritten <n>`, where n = give-ups − 64;
  - the summary line stays under 1024 bytes;
  - uevents at most one per second after the first;
  - the kernel log is still usable.

### 9.4 Not-covered proofs

With the fallback on, each asserts the same result as its strict arm, and the absence of any fallback event:

- `replace_marks_kept.sh`: the marks are not spent and the replace finishes correctly;
- `replace_abort.sh`;
- `replace_target_fail.sh`;
- `scrub_uncommitted.sh`;
- `stale_read.sh`;
- `torn_present.sh`: kept_torn is not spent and the reads that failed still fail;
- `two_stale.sh`;
- `unprovable.sh`;
- `mixed_unchecked.sh`;
- `rmw_inflight.sh`;
- `repair_pin.sh`;
- `verdict_keep.sh`;
- `recover_scrub.sh`: the MR2 plan still gives `recovery_log_full`;
- `flush_wedge.sh torn`, with `raid56_wf_readd_acks_unnamed=1` only to create the records: torn-only records are not spent.

### 9.5 Whole-suite diff and builds

- Run `regress.sh` twice: default, and with `OPTS+=,raid56_write_intent_policy=fallback`. Diff the outcomes. Every difference must be an arm listed in section 3.3. Any other difference means the flag has leaked into a site it must not reach.
- Run the selftests on a DEBUG and a non-DEBUG kernel.
- Run the fallback arms whose faults come from device removal, dm-flakey or null_blk (`degraded_log_full column/return/hotunplug`, `readd_flush`, `fullstripe_flush`, `detach_flush`, `policy_switch unwedge`, `fallback_flood`) also on a **CONFIG_BTRFS_DEBUG=n** UML kernel, built as af 8f5d2d78aa builds its second kernel. On production builds these paths are new live code, and printk ratelimiting differs there.
- Printk check on the non-DEBUG kernel: burst 200 btrfs crit lines from a second filesystem, then trigger a give-up. `raid56_given_up`, the uevent and `raid56_health` must still carry it.
- Model (optional): add to `tools/testing/btrfs/models/policy/policy_model.py` a check for a FALLBACK policy (A2 drop test without the taint, plus memory names). No silent state may be reached without a latched and persisted event on the same path, and the only silent classes reachable are "the disk returns" and "a crash or unmount before the repair".

---

## 10. Implementation plan

Prerequisite: the strict2 merge of af, ar and al into strict-wip (done). All lanes branch from that head.

### Lane P: policy plumbing, alert machinery, strict texts, docs

It touches:

- `fs/btrfs/super.c`, `fs/btrfs/fs.h`, `fs/btrfs/sysfs.c`;
- the alert region of `fs/btrfs/raid56-wib.c`: event tables, `raid56_alert*`, `raid56_alert_explain`, `raid56_alert_work`, `btrfs_raid56_health_show/_ack`, `raid56_latch_note_dev`, `raid56_latch_restore`;
- `fs/btrfs/raid56-wib.h` (events, policy, ring, hint struct);
- `raid56_wib_dump.py`, the rst, the README, COMPARISON.md;
- new UML drivers `policy_switch.sh` and `fallback_flood.sh`, and the fallback arms of `alert.sh` and `alert_latch.sh`;
- selftests ST-9 and ST-10.

**P0 lands first.** It is small and freezes the interface for the other lanes:

- `enum btrfs_raid56_policy`, `fs_info->raid56_policy`, `btrfs_raid56_fallback()`;
- option parse, show and emit, the remount early apply with its knob, and the sysfs file;
- `btrfs_raid56_set_policy()`;
- `wib->fallback` sampling in `wib_policy_locked()`;
- events 23 and 24 with names and the latched set;
- `enum raid56_giveup_cause`, `struct btrfs_wib_refusal_hint wib->hint`, and `raid56_fallback_gave_up()`, which already latches and appends to the ring; printing comes in P1;
- `raid56_wf_fallback_silent`;
- `btrfs_wib_dev_absent()`;
- the init-final3.sh helper `fallback_check <event> <logical>`, which does assertions (b)-(e).

**P1** (after P0, in parallel with E and N):

- the ring file;
- alert-work printing (explanations, one-liners, summary, reminder, U1 helper);
- fserror, uevent vars, health lines/state/action/caution;
- latch devid priority and restore crit;
- all of section 6 (it reads `wib->hint`; E and N fill it);
- docs;
- the tests listed above.

### Lane E: absent masks and C1 (eviction, replay, recovery)

It touches:

- the eviction region of `raid56-wib.c`: `wib_may_evict_naming` (unchanged), `wib_policy_locked` (`nr_absent`), `wib_entry_absent_only`, `wib_evictable`, `wib_evict_sticky`, `wib_count_entries_locked`, `wib_write_frees_room`, `wib_room_source_locked`;
- `btrfs_wib_mark` (fills the LOG_FULL hint; the S-c errno);
- `btrfs_wib_mark_stale` and `btrfs_wib_update_stale_parity` (absent parameter);
- `wib_name_region` (split absent/absent_par);
- `wib_add_sticky` (the recovery_full hint);
- `wib_readd_stale` and `wib_readd_record` (masks at recovery);
- the entry field lifecycle (clear sites);
- `fs/btrfs/raid56.c`: `rmw_update_stale_data` and `rmw_update_stale_parity` call sites only;
- `raid56-wib.h`: `struct btrfs_wib_entry` fields;
- selftests ST-1, ST-2, ST-3 and ST-11;
- UML: `degraded_log_full.sh` (fallback, unrelated, return, scan, hotunplug), `replay_full.sh` degraded, `commit_full.sh` degraded and verdict, and the not-covered proofs `verdict_keep`, `replace_marks_kept` and `torn_present`.

`wib_name_region`'s absent split is also used by lane N. Put it in E's first commit and merge that commit early, or have N rebase onto it.

### Lane N: memory names and C2 (failed-flush naming)

It touches:

- the side-table region of `raid56-wib.c`: `wib_refused_name_locked`, `wib_refused_any`, `wib_refused_stale_locked`, the consumers `btrfs_wib_stripe_error` and `btrfs_wib_recorded`, the retirement sites `btrfs_wib_clear_stale`, `btrfs_wib_clear_sticky` and `btrfs_wib_forget_range`, re-entry in `btrfs_wib_commit`, `btrfs_wib_unmount` and `btrfs_wib_remount_ro`, overflow handling, and the `names_in_memory` accessors for P;
- `wib_readd_dropped` (fallback refuse decision, S-a, hint);
- `btrfs_wib_commit` (owed branch; the lost-track latch stamp);
- `wib_unlogged_name` (partial naming, S-b, `wib_failed_holds_raid56`);
- `struct btrfs_wib` side-table fields;
- selftests ST-4, ST-5, ST-6, ST-7 and ST-8;
- UML: `flush_wedge.sh` (unnamed, unmount, crash, named, overflow), `readd_flush.sh`, `torn_readd.sh`, `fullstripe_flush.sh` (refuse, unknown, nonmember), `detach_flush.sh`, `refusal_marks.sh`, and the readd_nameless plan.

### Order and merge

1. P0, then E's first commit (the `wib_name_region` split and the entry fields).
2. P1, E and N in parallel.
3. Merge in the order P1, E, N. The expected conflicts are:
   - field additions to `struct btrfs_wib` and `struct btrfs_wib_entry`: E and N add different fields; P adds the ring and hint;
   - `init-final3.sh` case arms: every lane appends its own modes and plans; none edits an existing mode's body except to add a `fallback` CONTROL branch;
   - `README.md` tables.
4. On the merged tree:
   - build DEBUG and non-DEBUG, W=1, checkpatch;
   - run the selftests with the policy on and off;
   - run the full `regress.sh` diff (9.5);
   - run every new arm on both kernels where 9.5 says so.
5. Review pass, focused on:
   - the consumer checklist of section 3.2 item 2;
   - the latch invariant (R3);
   - that no fallback path runs with a knob's defect (`commit_prepare`, the skipped settle, the silent early return, the blind MR2 scrub).

---

## 11. Risks and residuals (the docs must state them)

1. **A dropped disk that comes back is protected only by the alert.**
   - C1 is A2's drop test without A2's taint.
   - A reboot with the disk attached serves its stale nodatasum data with no error.
   - The persisted latch, the crit restore line and the health `caution` and `action` lines say what to do.
   - Nothing enforces it, and an ack removes even that.
   - This is Q1.
2. **Memory names do not survive a crash.**
   - Exposure lasts until the repairs or a scrub land. It is permanent for a device that keeps failing writes, whose repairs never land.
   - Re-entry at each commit and at unmount narrows it.
   - Overflow brings back the strict refusal.
   - A missed consumer (section 3.2 item 2) would silently recreate stage 0. The checklist and ST-7 exist for that.
3. **K5 lost-track** is the one covered case where the kernel acknowledges a loss it cannot locate. It is bounded by the window since the device's last confirmed flush.
   - Follow-up (strict): force a flush of every device before the unlogged table (165 regions) overflows, so that "table full" stops being a source.
4. **Dead code going live.** The fallback paths are new live code on production kernels. Hence the non-DEBUG UML runs and the flood test.
5. **The fallback still refuses** in these cases, and a user told "it keeps running" will meet them:
   - a log full of a present failing device's records;
   - a degraded crash-mount whose verdicts and kept-torn records exceed a wide block;
   - a log that cannot be written (FL-15/16/20);
   - memory-name overflow;
   - every read, scrub and replace refusal.

   The texts say "does not change this" in each case.
6. **fstab.** A fallback left in fstab turns the user's rule around for every future failure. It is mitigated by:
   - the warn line at every mount;
   - the `policy` line in `raid56_health` and the POLICY uevent variable;
   - the hourly reminder while unacknowledged;
   - the docs recommending per-incident use.
7. **Evidence can be lost.**
   - An older kernel mounting read-write masks bits 23 and 24.
   - The ring is memory-only.
   - The one-liners are ratelimited.
   - A btrfs-progs command that lists memory names, or an extension of the stale-stripes ioctl, would help later.
8. **The degraded write hole grows** while the fallback keeps writing into stripes with no redundancy. A crash during one of those writes loses data beyond repair. E23 says so.
9. **Latch capacity.** 7 of 32 bits stay free after this.
10. **Strict follow-ups** that would shrink the need for the fallback. They are out of scope here but recommended:
    - MR2: scrub with a temporary over-capacity record;
    - FF-12: count a pin as room that is coming;
    - FF-8: `persist_now` retries after the readd when the only failure is an already-absent device;
    - the K5 forced flush.

---

## 12. Open questions for the user

- **Q1. Fence a disk whose records were dropped?**
  - As specified, when the fallback drops the records naming a missing disk and that disk later comes back (reconnected, or found at a reboot), the kernel only warns loudly (crit at mount, `caution`, `action`) and uses the disk. Its stale nodatasum data can read back old with no error.
  - The alternative "acts up" instead: at mount, a latched `fallback_missing_dropped` naming a devid that is present makes the kernel refuse to use that disk. The mount goes degraded without it, or fails with a clear text if that is beyond tolerance, until the devid is replaced.
  - That needs no new on-disk format (the latched devid already persists) but changes mount behaviour. It would ship as a follow-up if wanted.
  - Which do you want?
- **Q2. Should the fallback ever let a read return unchecked data?**
  - As specified, the fallback never makes a read return data the kernel knows to be stale or unchecked.
  - So it keeps refusing where stage 0 did exactly that: a present failing device's records while another device is missing, the tree-log replay with every device present, the mount's verdicts and torn marks.
  - The biggest cost is a degraded crash-mount with more undecided stripes than a log block holds: it stays read-only (copy the data off) instead of mounting read-write with those stripes' missing-column reads returning unchecked rebuilds, each announced.
  - Do you want any of those back under the flag? Each would be a separate, additive predicate with its own alert.
