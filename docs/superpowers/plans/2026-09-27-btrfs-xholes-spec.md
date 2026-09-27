# SPEC: closing the three btrfs-wide silent-loss holes X-1, X-2, X-3

This is an implementation spec for the strict-wip branch of /home/user/BTRFS-am.
Nothing here has been built or run by this spec's author. The only runs cited
are the earlier research runs in scratchpad/x1rig and the model copy in
scratchpad/x1model.

## 0. Ground rules, base, and how to read this

### 0.1 Base and line numbers

- Base: BTRFS-am strict-wip HEAD **149f0b7ae4**.
- The generic files this spec touches are byte-identical to 786ef3eeaa, the tree the attack round read:
  - disk-io.c, volumes.c, volumes.h, transaction.c, tree-log.c, dev-replace.c, tree-checker.c, fs.h, disk-io.h, file.c;
  - super.c differs only in btrfs_remove_bdev (+7 lines);
  - disk-io.c differs only in close_ctree (+3 lines) from 61b9dce00a.
- Every `file:line` below is at HEAD 149f0b7ae4. Upstream df2908090c is in /home/user/BTRFS-up; lines shown as `up:NNN`.

### 0.2 The user's rules as applied here

- The rules are: never lose data silently; clear, loud alerts; no lockups.
- Every behaviour change gets a CONFIG_BTRFS_DEBUG knob that restores the old behaviour:
  - declared with `module_param_named`, mode 0644;
  - with a `MODULE_PARM_DESC` that says "testing only: restores a known ... defect";
  - with an accessor `bool btrfs_<name>(void)` that is a `static inline false` without CONFIG_BTRFS_DEBUG;
  - following the pattern of bio.c:134-145, `split_bio_status_legacy`.
- Every fix gets a UML test in which:
  - the control arm (knobs on) reproduces the old loss;
  - the fixed arm shows the fix.
- Module parameters are named `btrfs.<name>`. The names below drop a redundant `btrfs_` prefix: `btrfs.replace_accepts_old_disk`, not `btrfs.btrfs_replace_accepts_old_disk`.

### 0.3 Arm scheme used by every UML test in this spec

Three "restore old" knobs exist, one per hole:
- X-1: `btrfs.log_commit_acks_missed_super=1`
- X-2: `btrfs.mount_reuses_generation=1`
- X-3: `btrfs.replace_accepts_old_disk=1`

| Arm | Kernel command line | Purpose |
|---|---|---|
| CONTROL | all three `=1` | Upstream behaviour for all three holes. Must reproduce the loss. |
| FIXED | none | The shipped default. |
| ISO-Xn | the other two `=1`, Xn's knob off | Shows Xn's own fix closes its hole without help from the others. This matters because X-2's mount commit alone also closes X-1's degraded arms. |
| UP | CONTROL scenario on uml-up (df2908090c) | Upstream reference. Run it once per arm, before any fix code exists. |

Verdict words used by every scoring script:
- **LOST**: an acknowledged file is missing or has the wrong md5, and no dmesg line names it.
- **CONTROL-LOUD**: the wrong lineage or disk was taken, but an error names it.
- **OK**, **LOUD_REPAIRED**.
- **INCONCLUSIVE**: a precondition failed. It is never a pass.
- **n/a**: for example, strict RAID56 refusal failed the fsync first, so nothing was acknowledged.
- **KNOWN_GAP**: a residual this spec documents and does not fix.

A pair passes only when CONTROL is LOST (or CONTROL-LOUD where stated) and FIXED is OK with the expected new alert line. A FIXED OK without the alert is a FAIL, because the data survived by luck.

Every run also:
- fails on `KERNEL_SPLAT` or `WATCHDOG`;
- filters the uml-s0w self-test noise `(efault) ... raid56 write-intent log still full`.

### 0.4 Decisions that need the user (defaults chosen; each has a knob)

1. **X-2 records its "generation fence" in the superblock field `__unused_log_root_transid`.** This is the uapi field at btrfs_tree.h:695.
   - No feature bit is set. Old kernels and btrfs-progs neither read nor check the field and carry it forward unchanged (verified by grep, §2.2).
   - Without it, a crash during the mount-time commit followed by a device swap brings X-2 back, on RAID1/RAID5 with 3 or more disks.
   - The attack's fallback, a large random skip, does not work. The orphan's random generation is above the new session's about half the time, and then it wins outright (§2.2 D2).
   - So the choice is:
     - use the field (recommended); or
     - accept the residual and document it.
2. **X-2 fences every read-write mount and ro→rw remount, not only degraded ones.**
   - Each one costs one extra superblock write per device plus one transaction commit.
   - This closes orphans left by a crashed `btrfs device add` or `btrfs replace start`. The older lineage does not know the new disk, so the next mount is not degraded.
   - The alternative, degraded-only (knob `gen_fence_degraded_only`), leaves those cases open.
3. **X-3 stores a per-devid lower bound in the existing dev item `generation` field** ("expected generation for this device").
   - For non-seed devices the field is always 0 and is read by nothing (verified).
   - Upstreaming needs agreement on this reading of the field.

Two defaults I chose that the user may want to know about. Both have a knob, and neither needs a decision:
- If a present device cannot take the X-2 fence record, the mount **stays writable** and warns loudly, rather than refusing read-write. Refusing would block `btrfs replace` of that very disk.
- X-3's fence is a hard refusal of the old disk even when it is the only way left to read nodatasum data. The escape is offline `btrfs restore`, which is documented.

---

## 1. X-1: a log commit's superblock misses a device, which then wins the generation tie

### 1.1 Confirmed

**Status: CONFIRMED.**
- I re-read the code at HEAD.
- The research reproduced it on uml-up (df2908090c) and uml-s0w (61b9dce00a) in the super0, dev0, multi0 and degr0 arms. It is LOST on raid1 x2, raid5:raid1 x3, raid5:raid5 x3 and raid6:raid6 x4 (super0).

The chain I checked:
1. The commit writes gen N with log_root 0 (transaction.c:2549-2552) to every device.
2. A later fsync reaches btrfs_sync_log. It puts log_root into super_for_commit with gen still N (tree-log.c:3550-3551), then calls `write_all_supers(trans)` (tree-log.c:3552).
3. write_all_supers tolerates the miss:
   - `max_mirrors = 1` for log commits (disk-io.c:4204-4206);
   - `max_errors = num_devices - 1` (4223);
   - a `!bdev` device is counted silently (4255-4258);
   - a `!IN_FS_METADATA || !WRITEABLE` device is skipped and not counted (4259-4261);
   - `total_errors` is reset between the submit and wait loops (4303);
   - it returns 0.
4. btrfs_sync_log then sets `last_log_commit` (tree-log.c:3572) and the fsync returns 0.
5. At mount:
   - list_sort by devid (volumes.c:1322);
   - `>` on generation (volumes.c:1241-1248) picks the lowest devid on a tie;
   - open_ctree reads only that device's primary (disk-io.c:3413);
   - log_replay = log_root != 0 (3743), so it is false;
   - the fsync'd data is gone, with no message.
6. It is the same in upstream: btrfs_sync_log and open_fs_devices are byte-identical; write_all_supers is up:4133.

### 1.2 Disputed points, checked in code

| # | Claim (attack) | Verdict | Evidence |
|---|---|---|---|
| D1 | An absent replace target (devid 0) is on no list, sorts first, and so wins the tie. This applies to single-device filesystems too. | **Confirmed** | btrfs_init_dev_replace lets `tgtdev == NULL` through with `-o degraded` (dev-replace.c:119-126). There is no dev item for devid 0 (`WARN_ON(devid == BTRFS_DEV_REPLACE_DEVID)` in fill_device_from_item, volumes.c:7819). The sort and pick are at volumes.c:1322 and 1247. |
| D2 | Mirror copies never carry a log root, so the invariant holds for primaries only. | **Confirmed** | max_mirrors = 1 for log commits (disk-io.c:4204-4206). Documentation only, plus a progs follow-up. |
| D4 | usebackuproot rolls back the super_copy generation between the proposed init (3599) and init_dev_replace (3670). | **Confirmed** | read_backup_root calls `btrfs_set_super_generation(super, ...)`. It runs inside init_tree_roots (disk-io.c:3619). |
| D6 | The detach note would name devid 0 after a replace. | **Confirmed** | `tgt_device->devid = src_device->devid; src_device->devid = 0` (dev-replace.c:1015-1016) comes before btrfs_rm_dev_replace_remove_srcdev (1035). |
| D8 | The "read-only" reason cannot happen on a read-write mount. | **Partly** | bdev_file_open_by_path returns -EACCES for a read-only bdev opened for write (block/bdev.c:1128-1132), so a rw mount sees it as missing. The reason is reachable through `mount -o ro` then `remount,rw`: btrfs_open_one_device clears WRITEABLE for a read-only bdev (volumes.c:702-703), and btrfs_remount_rw does not reopen devices. Keep the reason and add an arm. |
| D7 | Zoned devices advance the sb-log write pointer at submit time. | Not verified | Listed as a test gap (zoned run). |
| concurrency | A forced path that set last_log_commit would let a same-inode fsync return early. | **Confirmed** | btrfs_inode_in_log needs `last_sub_trans <= btrfs_get_root_last_log_commit(root)` (btrfs_inode.h:458-468), used by skip_inode_logging (file.c:1495-1502). Other roots' waiters take `root_log_ctx.log_ret` and never set their own last_log_commit (tree-log.c:3440-3460). BTRFS_LOG_FORCE_COMMIT is non-zero (tree-log.h:36), and btrfs_sync_file commits on any non-zero return. |

### 1.3 Final fix, function by function

**Rule (erratum 6 / model F1-any, plus missing and off-list holders):** a tree-log commit must not acknowledge an fsync while any device of the filesystem may still hold a primary superblock of this generation that this log commit's superblock did not replace. In that case btrfs_sync_log returns BTRFS_LOG_FORCE_COMMIT, and the fsync commits the transaction.

A device "may hold" a generation if either:
- its primary at open, or the highest generation *submitted* to it since, is at least that generation (submitted, not completed, because a failed or timed-out FUA may have landed); or
- it is absent from the device list but may carry this filesystem's superblock:
  - an unopened member is assumed to carry `max(scanned gen, mount gen)`;
  - a detached member keeps its high-water mark until a commit passes it;
  - a missing replace target is assumed to carry the mount generation.

Invariant: every **primary** superblock at the highest on-disk generation contains every acknowledged fsync. The devid tie-break at mount therefore stays as it is.

**1. fs/btrfs/volumes.h** (fields go in via Lane 0; see §6)
- `struct btrfs_device`: `u64 sb_generation;`, commented as: highest superblock generation this device may hold (primary at open, else raised at every write_all_supers SUBMIT); protected by device_list_mutex.
- `struct btrfs_fs_devices`:
  - `u64 mount_sb_generation;`: super_copy generation as read at mount, before any usebackuproot rollback;
  - `u64 absent_sb_generation; u64 absent_devid; const char *absent_why;`: a superblock of this fs up to this generation may exist on a device not on `devices`; protected by device_list_mutex.
- Prototype: `void btrfs_note_absent_super(struct btrfs_fs_devices *fs_devices, u64 gen, u64 devid, const char *why);`

**2. fs/btrfs/volumes.c: `btrfs_note_absent_super()`** (Lane 0)
```c
void btrfs_note_absent_super(struct btrfs_fs_devices *fs_devices, u64 gen,
			     u64 devid, const char *why)
{
	lockdep_assert_held(&fs_devices->device_list_mutex);
	if (gen < fs_devices->absent_sb_generation)
		return;
	fs_devices->absent_sb_generation = gen;
	fs_devices->absent_devid = devid;
	fs_devices->absent_why = why;		/* static string */
}
```
Callers go immediately before each `list_del_rcu` of a member. device_list_mutex is already held at every site. Skip the call when `dev->fs_devices != fs_info->fs_devices` (seed devices never get this fs's superblocks).

| Site | Arguments |
|---|---|
| btrfs_rm_device, before volumes.c:2439 | `(device->sb_generation, device->devid, "being removed")` |
| btrfs_destroy_dev_replace_tgtdev, before volumes.c:2590 | `(tgtdev->sb_generation, 0, "replace target removed")` |
| btrfs_init_new_device error path, before volumes.c:3132 | `(device->sb_generation, device->devid, "add failed")` |
| btrfs_dev_replace_finishing, before dev-replace.c:1035 (owned by Lane C, which reworks this function) | `(src_device->sb_generation, tgt_device->devid, "replaced, not committed")`. Pass the **target's** devid, which now holds the source's old devid (D6). |

A comment at write_all_supers says: "any new path that takes a member off `devices` must call btrfs_note_absent_super() first."

**3. fs/btrfs/disk-io.h / disk-io.c: `write_all_supers(trans, miss)`** (signature change in Lane 0; body in Lane A)
```c
struct btrfs_super_miss {		/* filled for log commits only */
	int nr;				/* devices that may hold this generation without this sb */
	u64 devid;			/* the first of them */
	const char *why;		/* "write failed" | "missing" | "read-only" | "being removed" | an absent_why */
	bool zoned;
};
int write_all_supers(struct btrfs_trans_handle *trans, struct btrfs_super_miss *miss);
```
- The transaction commit (transaction.c:2607) passes NULL; btrfs_sync_log passes `&miss`.
- In the body, `const u64 gen = btrfs_super_generation(sb);` after `sb = fs_info->super_for_commit`.
- Submit loop (4254-4287): after the btrfs_validate_write_super check and before write_dev_supers():
  `dev->sb_generation = max(dev->sb_generation, gen);`
- Wait loop (4304-4315): rewrite it with the same filtering, tracking `landed`:
```c
	list_for_each_entry(dev, head, dev_list) {
		bool landed = false;

		if (dev->bdev &&
		    test_bit(BTRFS_DEV_STATE_IN_FS_METADATA, &dev->dev_state) &&
		    test_bit(BTRFS_DEV_STATE_WRITEABLE, &dev->dev_state)) {
			ret = wait_dev_supers(dev, max_mirrors);
			if (unlikely(ret))
				total_errors++;
			else
				landed = true;
		}
		if (miss && !landed && dev->sb_generation >= gen)
			btrfs_super_miss_add(miss, dev->devid,
				!dev->bdev || test_bit(BTRFS_DEV_STATE_MISSING, &dev->dev_state) ? "missing" :
				!test_bit(BTRFS_DEV_STATE_IN_FS_METADATA, &dev->dev_state) ? "being removed" :
				!test_bit(BTRFS_DEV_STATE_WRITEABLE, &dev->dev_state) ? "read-only" :
				"write failed");
	}
	if (miss && fs_info->fs_devices->absent_sb_generation >= gen)
		btrfs_super_miss_add(miss, fs_info->fs_devices->absent_devid,
				     fs_info->fs_devices->absent_why);
```
- `landed` means that wait_dev_supers returned 0, which for `max_mirrors == 1` means the primary FUA completed (disk-io.c:4010-4050).
- Unchanged: max_errors, the abort paths, the barriers, btrfs_wib_commit.

**4. fs/btrfs/disk-io.c: `open_ctree()`**
- New static `btrfs_init_sb_generations(fs_info)`, called right after the latest_dev check (after 3604). At this point the device list is final and still holds devid 0; no superblock has been written and usebackuproot has not rolled back yet.
```c
	const u64 gen = btrfs_super_generation(fs_info->super_copy);
	mutex_lock(&fs_devices->device_list_mutex);
	fs_devices->mount_sb_generation = gen;
	fs_devices->absent_sb_generation = 0;		/* fs_devices outlives mounts */
	fs_devices->absent_devid = 0;
	fs_devices->absent_why = NULL;
	list_for_each_entry(dev, &fs_devices->devices, dev_list)
		dev->sb_generation = dev->bdev ? dev->generation	/* primary, volumes.c:689 */
					       : max(dev->generation, gen);
	mutex_unlock(&fs_devices->device_list_mutex);
```
- Right after btrfs_init_dev_replace (after 3674): if `btrfs_dev_replace_is_ongoing(&fs_info->dev_replace) && !fs_info->dev_replace.tgtdev`, then under device_list_mutex call `btrfs_note_absent_super(fs_devices, fs_devices->mount_sb_generation, 0, "replace target missing")`. This is D1.
  - A target that is on the list but has no bdev is already covered by the per-device init above.

**5. fs/btrfs/tree-log.c: `btrfs_sync_log()`** (3550-3574)
```c
	struct btrfs_super_miss miss = {};
	...
	ret = write_all_supers(trans, &miss);
	mutex_unlock(&fs_info->tree_log_mutex);
	if (unlikely(ret)) { ...unchanged abort path... }

	if (unlikely(miss.nr) && !btrfs_log_commit_acks_missed_super()) {
		btrfs_log_commit_miss_warn(fs_info, &miss,
			btrfs_super_generation(fs_info->super_for_commit));
		btrfs_set_log_full_commit(trans);	/* before anyone is woken */
		ret = BTRFS_LOG_FORCE_COMMIT;
		trace_btrfs_sync_log_exit(trans, root, ctx, ret);
		btrfs_log_commit_miss_pause(fs_info, &miss);	/* test hook */
		goto out_wake_log_root;	/* waiters get ret via btrfs_remove_all_log_ctxs */
	}
	ASSERT(...);				/* unchanged */
	btrfs_set_root_last_log_commit(root, log_transid);	/* ONLY on the acknowledged path */
	if (unlikely(miss.nr))
		btrfs_log_commit_miss_pause(fs_info, &miss);	/* legacy arm: same window */
```
- Taking out_wake_log_root after `log_root_tree->log_transid++` matches the existing -EIO path (tree-log.c:3535-3547).
- btrfs_log_commit_miss_warn is `btrfs_warn_rl` (text in §1.5).
- btrfs_log_commit_miss_pause is `msleep(min(pause_ms, 30000))` plus a btrfs_info line. It holds only the transaction handle.

**6. fs/btrfs/transaction.c:2607**: `write_all_supers(trans, NULL)` (Lane 0).

Deliberately not done:
- No mount tie-break. It cannot order two log roots of one generation (X-1b); `btrfs rescue zero-log` writes same-generation supers; and usebackuproot and X-2 break "same generation means same lineage".
- No mount-time "superblocks disagree" alert. A power cut between the parallel super writes produces the same state on healthy disks.
- No format change.

### 1.4 Knobs (tree-log.c, CONFIG_BTRFS_DEBUG)

- `btrfs.log_commit_acks_missed_super` (bool, 0644, default 0).
  - Description: "Acknowledge an fsync whose tree-log superblock missed a device that may hold a superblock of the same generation, as before (testing only: restores a known silent-loss defect)".
  - At 1, btrfs_sync_log ignores `miss.nr`. The bookkeeping still runs but decides nothing. This one knob restores the old behaviour for failing, missing, read-only, detached and absent-target devices alike.
- `btrfs.log_commit_miss_pause_ms` (uint, 0644, default 0, **clamped to 30000 when read**; the clamp is stated in the description). Test hook: it sleeps in the window after the decision, in both arms, and prints `log commit missed devid %llu: pausing %u ms before waking fsyncs (testing)`.
- Shared test hook (Lane 0, disk-io.c): `btrfs.sb_crash_after_devid` (long, 0644, default -1, one-shot via `xchg(..., -1)`).
  - At the next write_all_supers of a **transaction commit**, it submits and waits for the supers of that devid only, then panics with `btrfs: sb_crash_after_devid: superblock of generation %llu written to devid %llu only`.
  - If that devid is not on the list, it panics after the barriers with no super written.
  - X-2's chain, addcrash and tgtcrash arms use it.

### 1.5 Alerts

- **New (the one place X-1 forces something):** `btrfs_warn_rl` in btrfs_sync_log, once per forced commit:
  `fsync: devid %llu did not get the tree-log superblock of generation %llu (%s)%s and may still hold a superblock of that generation without the log, which a mount after a crash could use; committing the transaction instead%s`
  - The first `%s` is the reason: write failed | missing | read-only | being removed | replaced, not committed | replace target removed | replace target missing | add failed.
  - The second `%s` is `" and %d more device(s)"` when `nr > 1`.
  - The tail depends on the reason:
    - "write failed": `"; every fsync commits the transaction while it keeps failing superblock writes -- replace it"`, with `" or remount"` added when zoned;
    - "missing" or "replace target missing": `"; once per mount"`;
    - otherwise empty.
- Existing lines that now pair with it:
  - `lost super block write due to IO error on %s` (disk-io.c:3894);
  - `error writing primary super block to device %llu` (4048);
  - the dev-stats write-error counter.
- Debug only: the pause line from §1.4.

### 1.6 On-disk impact

None:
- the fields are in-memory only;
- the forced commit writes exactly what an fsync falling back to a full commit already writes;
- old kernels and progs read the result unchanged.

### 1.7 UML reproduction: `tools/testing/btrfs/uml/log_super_tie.sh <kernel> [arm...]`

This is a port of scratchpad/x1rig (x1init.sh, x1run.sh). PROFILE and NDEV come from the environment. It uses the shared rig additions in §5.

New init-final3.sh MODEs:
- **tie_write**: scenario boot on dm devices.
  1. `mkfs` over d0..dN in order, so d0 is devid 1. Log `btrfs fi show`.
  2. Mount `-o commit=300`.
  3. Write `base`, then `sync`, and log gen N.
  4. For multi0, `fsync_file f1`.
  5. Inject per ARM.
  6. `fsync_file f2`, and log the fsync rc.
  7. `kmsg "lost super|primary super|errs:|did not get the tree-log"`.
  8. `echo o > /proc/sysrq-trigger` (no finish(), because it syncs).
- **tie_d1** (mkfs on raw ubd, write, umount).
- **tie_d2** (degraded boot: `-o degraded,commit=300`, fsync f2, crash, no sync).
- **tie_pause**, **tie_repeat**, **tie_rm**.
- **tie_repl_w** / **tie_repl_d** (absent replace target).
- **tie_rorm** (read-only member).
- **tie_verify**: raw ubd, no dm.
  1. `sb_info` on every device before mounting.
  2. PRECOND (CONTROL): every device at the same generation, the failed device with log_root 0, and the others non-zero (multi0: all non-zero and different). If it fails, report INCONCLUSIVE.
  3. Mount, and grep `start tree-log replay` to log REPLAY yes or no.
  4. verify_manifest.
  5. `umount`, then `btrfs check --readonly` (expect rc 0).

Arms. Profile r1 = raid1:raid1 x2 unless noted. Each arm runs CONTROL, FIXED and ISO-X1, plus UP once.

| Arm | Scenario | CONTROL / UP | FIXED | ISO-X1 |
|---|---|---|---|---|
| nofault | fsync, no fault | REPLAY yes, f2 OK | same; no warning; all devices at N with the same log_root (no forced commit) | same |
| super0 | `dm_fail_primary_super 0`, then fsync f2, then crash | devid1 (N,0), devid2 (N,X); REPLAY no; f2 LOST; check rc 0 | warning `devid 1 ... (write failed)`; devid1 at N, devid2 at N+1; f2 OK | same as FIXED |
| super1 | same with devid 2 failing | f2 OK (tie-break control) | forced commit shown (devid 2 warning); f2 OK | same |
| dev0 | `dm_error_writes 0` (flushes too) | LOST | as super0 | as super0 |
| multi0 (X-1b) | fsync f1; fail devid1's super; fsync f2; crash | REPLAY yes, f1 OK, f2 LOST | f1 and f2 OK | same |
| degr0 | tie_d1; tie_d2 omitting ubd0; verify with every device | LOST, no message in any boot | f2 OK; boot 2 shows the **X-2** fence line (mount commit), no X-1 warning needed | boot 2 shows `devid 1 ... (missing)` at the fsync; f2 OK |
| degr1 | omit ubd1 | OK | OK | OK, forced commit shown |
| pause | `log_commit_miss_pause_ms=4000`; super0 fault; fsync #1 of f2 in background; at +1 s `sync $MNT/f2` in background (logs FSYNC2_OK); crash at +3 s. The pause line must appear **before** fsync #2 starts, else INCONCLUSIVE. | FSYNC2_OK during the pause; f2 LOST | neither fsync returned before the crash (FSYNC2_OK means FAIL) | same |
| pause-live | same, no crash | both return | both return within pause+10 s; gen advanced; no WATCHDOG | same |
| repeat | devid1 super failing; 20 x (small file + fsync); crash | gen unchanged; files LOST | all 20 returned; gen +>=20; warning rate-limited; files OK; wall time logged | same |
| rm | raid1 x3; `dm_delay_supers 0 3000`; `btrfs device remove 1` in background; once `/sys/fs/btrfs/<fsid>/devinfo/1` disappears, `fsync_file f2`; crash when the fsync returns or at 30 s | devid1 (N,0) with its scratch still queued; others (N,X); f2 LOST. Run UP first: this CONTROL result is inferred from code. | warning `(being removed)`; f2 acknowledged only after the remove's commit, so devid 2 and 3 are at N+1 and f2 is OK (or f2 was never acknowledged). A scratched, unreadable super counts as "no super", not INCONCLUSIVE. | same |
| repl_absent (a: raid1 x2 + target; b: single + target) | tie_repl_w: fill 400 MiB; `btrfs replace start` in background; umount while below 100% (else INCONCLUSIVE); dump-super shows target devid 0 at G. tie_repl_d: host omits the target; log that a plain mount fails; `-o degraded,commit=300`; fsync f2; crash. | PRECOND target (G,0), members (G,X); REPLAY no; f2 LOST | X-2 fence line at boot B; f2 OK | `devid 0 ... (replace target missing)` at the fsync; members at G+1; f2 OK |
| rorm (optional) | `blockdev --setro /dev/mapper/d0`; `mount -o ro`; `remount,rw`; fsync f2; crash | LOST | warning `(read-only)`, or n/a if the remount is refused | same |

Branch profiles:
- super0, dev0, multi0 and degr0 also run on raid5:raid1 x3, raid5:raid5 x3 and raid6:raid6 x4.
- If strict refusal fails the fsync first (FSYNC_FAIL), the arm is n/a.

### 1.8 Model

The model run already done (scratchpad/x1model/c_model.py, a copy of attack/c_model.py):
- `--f1 none`: 11,816 SILENT_LOGROOT;
- `--f1 any --f1-missing`: 0;
- RAID6: 35,384 → 0;
- missing-device-returns family: `--f1 any` alone leaves 9,784, and adding `--f1-missing` takes it to 0.

Holders outside the device list (absent target, detach sites) are not modelled. The README says so; the UML arms rm and repl_absent cover them.

### 1.9 fstests (against the volume-s0e baseline; btrfs/100 and btrfs/146 already fail there)

- Multi-device dm-error fsync: btrfs/146, btrfs/160.
- The volume list in scratchpad/fstests/volume.list.
- Replace and remove under load: btrfs/011 027 064 065 069 070 071.
- The `log` group.
- Single-device dm-flakey fsync tests (must be unchanged, max_errors = 0): generic/034 039 040 041 056 059 065 066 311 321 322 335 336 341 342 343 348 376.
- Take the group membership in the fstests tree as authoritative if a number above has moved.

### 1.10 Risks and residuals

1. **Cost.**
   - While a device keeps failing primary super writes, every fsync is a full commit. The repeat arm measures this.
   - A timing-out disk costs the fsync the timeout twice.
   - Stage 1's failed state ends the cost: it stops submitting to F. Stage 1 must reuse `dev->sb_generation` instead of adding `last_super_gen_submitted`, and keep F counted while F is skipped.
2. **Concurrency** rests on two details: last_log_commit is not set on the forced path, and log_full_commit is set before the wake. The pause arm tests both.
3. **Completeness of the detach tracking** depends on every list_del_rcu site calling btrfs_note_absent_super. The comment at write_all_supers covers future sites. The runtime hot-unplug path (btrfs_remove_bdev) keeps the device on the list, so the main tally covers it; it has no arm.
4. **A failed forced commit** (strict refusal, ENOSPC, abort) returns an error from fsync while the log is durable elsewhere. It may reappear after a crash. It was never acknowledged, so this is acceptable.
5. **Filesystems already left in the bad state by an unfixed kernel** stay bad. Neither the mount side nor anything else can tell that state from an ordinary crash.
6. **Mirrors** (D2): the invariant covers primaries only. progs `super-recover` can still pick a mirror (N, 0) over a torn primary (N, X). This is a progs follow-up (§7).
7. **Zoned**: a failed sb-log write may leave the write pointer ahead (D7, not verified). Every fsync would then commit until remount. This is a known gap; the alert says "or remount".
8. **nobarrier**: "landed" is not durable. Upstream already documents nobarrier as unsafe; the README says X-1 does not cover it.

---

## 2. X-2: an orphan superblock from an interrupted commit, and a later session that reuses its generation

### 2.1 Confirmed

**Status: CONFIRMED by code reading in both trees. Not yet run.**
1. write_all_supers submits every device's superblock before waiting on any (disk-io.c:4254-4315). A power cut can leave N+1 on one device D only. Its tree blocks are already on every device, because btrfs_write_and_wait_transaction runs first (transaction.c:2594).
2. At the next mount without D:
   - init_tree_roots sets generation and last_trans_committed to N (disk-io.c:2755-2756);
   - join_transaction takes `generation + 1` (transaction.c:393-394), so the session commits N+1 again.
3. When D returns, the outcome depends on the number of degraded commits j:
   - j = 0: D's N+1 beats N outright, and the degraded fsyncs are gone;
   - j = 1: the generations tie and the lowest devid wins;
   - j >= 2: same-generation per-block collisions pass the transid check (disk-io.c:409-441), and scrub cannot see them (scrub.c:879 compares header gen with extent item gen).
4. Upstream is identical (up:4133, up:1240-1247, up:393, up:2049).

### 2.2 Disputed points, checked in code

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| P1 | A scan while mounted decrements `missing_devices` for a device that is never opened. | **Confirmed** | device_list_add, `!name \|\| devt differs` branch: MISSING is cleared and missing_devices decremented (volumes.c:959-962); bdev stays NULL; generation is updated only when `!opened` (972-976). btrfs_check_rw_degradable counts `!bdev` (volumes.c:8096-8098). Count absent devices from the list. |
| P2 | Tolerance-1 chain: a crash inside the fence commit, then a swap of which disk is absent, gives the same fence again. | **Confirmed by reasoning** | Supers are submitted in list order and the list is sorted by devid (volumes.c:1322, disk-io.c:4254). Both mounts see base N and absent = 1. **The attack's random-skip fallback is unsound:** mount 2 cannot bound mount 1's random F1, so the orphan beats the session outright in about half of the cases (`r1 > r2 + k`). A deterministic fix needs durable state on a device both mounts share. |
| P3 | Orphans on a new or target device, with a non-degraded next mount. | **Confirmed** | btrfs_init_new_device does list_add_rcu at the head (volumes.c:3013), and so does btrfs_init_dev_replace_tgtdev (dev-replace.c:323), so their supers are submitted first. The older lineage has no dev item for them. |
| P4 | btrfs_commit_gen_fence can commit an older running transaction and stop. | **Confirmed** | join returns the running transaction. Fix with a loop. |
| P5 | Scanned generations are unverified. | **Confirmed**, narrower than stated | btrfs_read_disk_super checks only magic and bytenr (volumes.c:~1370-1380). An **opened** device with a bogus high generation becomes latest_dev and fails the csum check at open_ctree (disk-io.c:3442), which is loud. Only scanned-but-unopened sources need the bound. The intent record must come from checksum-verified supers. |
| P6 | The fence commit is refused on a full write-intent log with a device missing. | **Mostly moot at HEAD, still measure** | wib_may_evict_naming (raid56-wib.c:923-933) lets a full log evict records naming a missing device while `missing_devices > 0`, which is errata 2. So the fence commit behaves like any first degraded commit, only earlier. The advice text change (nologreplay) is accepted. Side note: after P1's scan, `missing_devices` is 0 and that exception switches off; that is COMPARISON §7 row 18 and pre-existing. |
| P7 | Alert 3 reference is wrong after a replace-target pick, and noisy after ordinary crashes. | Accepted | Compare against `mount_sb_generation`. Soften the wording when exactly one behind. |
| P8 | commit_early_writeback breaks zoned. | Accepted | No-op on zoned. |
| - | The tree-checker `+1` bound is also checked at write time. | **Confirmed** | tree-checker.c:1234-1245 and 1330-1350; btree_csum_one_bio disk-io.c:295-314. super_copy's generation moves only in update_super_roots (transaction.c:1991, called at 2547). The tree root is always COWed (transaction.c:1377-1379), so an empty commit still advances the generation. |
| - | The replay reads the log root at `generation + 1`. | **Confirmed** | disk-io.c:2052. It is the only other `generation + 1` in fs/btrfs besides transaction.c:393 (grep). |
| - | `__unused_log_root_transid` is free. | **Confirmed** | There are no kernel references beyond uapi btrfs_tree.h:695. progs (scratchpad/btrfs-progs @fac7d38) only prints it, as `log_root_transid (deprecated)` (print-tree.c:2272). progs' write_all_supers writes its in-memory super_copy, which carries the field forward. btrfs check does not look at it. |
| - | btrfs_wib_rw_mount could commit before the replay. | **Checked: no** | raid56-wib.c has no start, join or commit transaction call. |

### 2.3 Final fix, function by function

**Rule.** A writable session never reuses a generation that any device of this filesystem may carry, whether the device is listed, absent, newly added, a replace target, or the holder of an interrupted fence commit.

Every writable mount, ro→rw remount, and read-only mount that replays a log:
1. **computes the fence F**:
   - `base = max(committed gen, mount_sb_generation, every listed device's generation, scan_generation and verified fence record)`;
   - `absent` = listed devices with `!bdev || MISSING`, plus 1 for an ongoing replace whose target is not listed;
   - `F = base + 1 + max(1, absent)`, with an overflow check;
2. **records F durably** in each present writable device's own primary superblock (field `__unused_log_root_transid`), before any block of the fence transaction can reach disk;
3. makes the **first transaction take transid F**, bumping super_copy's generation in memory for the tree-checker;
4. **commits it** before userspace can write or fsync.

A fence commit that crashes after some supers leaves F recorded on every present device. The next mount, which shares at least one device when the profile tolerates one loss and there are 3 or more devices, then computes a higher F. Split brain across disjoint device sets remains a residual.

**1. fs/btrfs/fs.h**: `u64 gen_fence;` in btrfs_fs_info, next to generation. It is the lowest transid the next new transaction may take; 0 means none.

**2. fs/btrfs/accessors.h**:
`BTRFS_SETGET_STACK_FUNCS(super_fence_intent, struct btrfs_super_block, __unused_log_root_transid, 64);`
In include/uapi/linux/btrfs_tree.h:695, add a comment only; do not rename the field: `/* since 7.x: generation fence intent, a lower bound for the next writable session (fs/btrfs/disk-io.c:btrfs_set_gen_fence) */`.

**3. fs/btrfs/volumes.h** (btrfs_device):
- `u64 scan_generation;`: the generation a scan found while the fs was open and the device was unopened;
- `u64 fence_rec;`: `__unused_log_root_transid` of this device's primary, read at open only if its checksum verifies.

**4. fs/btrfs/volumes.c**
- **device_list_add** (end of function, volumes.c:971-977):
```c
	if (!fs_devices->opened) {
		...unchanged...
	} else if (!device->bdev) {
		/* An absent member re-appeared while mounted (X-2 base). */
		device->scan_generation = max(device->scan_generation, found_transid);
	}
```
- **btrfs_open_one_device**, after the uuid match (volumes.c:686-689):
  `device->fence_rec = btrfs_super_csum_ok(disk_super) ? btrfs_super_fence_intent(disk_super) : 0;`
- **btrfs_close_one_device**: `device->scan_generation = 0; device->fence_rec = 0;`

**5. fs/btrfs/disk-io.c: `bool btrfs_super_csum_ok(const struct btrfs_super_block *sb)`**
- Returns false for an unsupported csum type (btrfs_supported_super_csum, disk-io.c:140).
- Otherwise it compares `btrfs_csum(btrfs_super_csum_type(sb), sb + CSUM, INFO_SIZE - CSUM, out)` against `sb->csum` over `btrfs_super_csum_size(sb)` bytes.
- It needs no fs_info, so it can be used at open time.

**6. fs/btrfs/transaction.c: `join_transaction()`**, replacing 393-394 (trans_lock is held):
```c
	u64 transid = fs_info->generation + 1;
	const u64 fence = READ_ONCE(fs_info->gen_fence);

	if (unlikely(fence > transid)) {
		/*
		 * First transaction after a fenced mount: skip to the fence.  The
		 * tree-checker bounds item generations by super generation + 1 at
		 * read AND write time, and update_super_roots() moves it only at
		 * commit, so raise it now, in memory only.  super_for_commit (what
		 * a log commit writes) keeps the committed value.
		 */
		transid = fence;
		btrfs_set_super_generation(fs_info->super_copy, transid - 1);
	}
	btrfs_set_fs_generation(fs_info, transid);
	cur_trans->transid = transid;
```

**7. fs/btrfs/disk-io.c: fence computation and intent record**
```c
#define BTRFS_GEN_PLAUSIBLE	(1ULL << 32)

int btrfs_set_gen_fence(struct btrfs_fs_info *fs_info)
{
	struct btrfs_fs_devices *fs_devices = fs_info->fs_devices;
	const u64 gen = btrfs_get_fs_generation(fs_info);	/* committed; no transaction yet */
	const u64 mnt = fs_devices->mount_sb_generation;	/* pre-usebackuproot */
	const u64 limit = max(gen, mnt) + BTRFS_GEN_PLAUSIBLE;
	const bool intent = !btrfs_gen_fence_volatile();
	u64 base = max(gen, mnt), absent = 0, fence;
	u64 absent_ids[4]; int nr_ids = 0;
	struct btrfs_device *dev;

	if (btrfs_mount_reuses_generation()) {
		btrfs_warn(fs_info, "testing: mount_reuses_generation=1: this mount starts at generation %llu, which an absent device may already carry", gen + 1);
		return 0;
	}
	mutex_lock(&fs_devices->device_list_mutex);
	list_for_each_entry(dev, &fs_devices->devices, dev_list) {
		if (!dev->bdev || test_bit(BTRFS_DEV_STATE_MISSING, &dev->dev_state)) {
			absent++;
			if (nr_ids < 4) absent_ids[nr_ids++] = dev->devid;
		}
		base = fold(fs_info, base, limit, dev->generation, dev->devid);
		base = fold(fs_info, base, limit, dev->scan_generation, dev->devid);
		if (intent)
			base = fold(fs_info, base, limit, dev->fence_rec, dev->devid);
	}
	mutex_unlock(&fs_devices->device_list_mutex);
	down_read(&fs_info->dev_replace.rwsem);
	if (btrfs_dev_replace_is_ongoing(&fs_info->dev_replace) && !fs_info->dev_replace.tgtdev) {
		absent++;				/* off-list target, devid 0 */
		if (nr_ids < 4) absent_ids[nr_ids++] = 0;
	}
	up_read(&fs_info->dev_replace.rwsem);

	if (btrfs_gen_fence_degraded_only() && !absent && base == gen)
		return 0;
	if (check_add_overflow(base, 1 + max_t(u64, absent, 1), &fence)) {
		btrfs_err(fs_info, "generation fence overflows (base %llu): read-write refused", base);
		return -EOVERFLOW;
	}
	WRITE_ONCE(fs_info->gen_fence, max(READ_ONCE(fs_info->gen_fence), fence));
	btrfs_set_super_fence_intent(fs_info->super_copy, fence);	/* the fence commit carries it everywhere */
	/* alert B1 / B1' / debug, see 2.5 */
	if (intent)
		btrfs_write_fence_intent(fs_info, fence);
	return 0;
}
```
`fold(v)`:
- If `v > limit`: log B5 once per devid and return base unchanged.
- Otherwise return `max(base, v)`.

**btrfs_write_fence_intent(fs_info, F)** (static, disk-io.c):
1. Hold device_list_mutex, with a kmalloc'd 4 KiB buffer.
2. For each device with `bdev && IN_FS_METADATA && WRITEABLE` (absent ones are already counted in F):
   1. `btrfs_read_disk_super(dev->bdev, 0, false)` → copy → release the page.
   2. Require:
      - `btrfs_check_super_csum(fs_info, buf) == 0`;
      - `dev_item.devid == dev->devid`;
      - dev_item uuid equal to dev->uuid;
      - fsid equal to super_copy's fsid;
      else report the failure as -EUCLEAN.
   3. If the device's field is already `>= F`, skip it.
   4. Otherwise set the field, then `write_dev_supers(dev, buf, 1)` and `wait_dev_supers(dev, 1)`. These are the existing static helpers: they recompute the checksum and handle the zoned sb-log location. Zone info is read at 3628, before this point.
   5. On any failure, print B2 and continue.
3. The device's **own** generation and root pointers are written back unchanged. A behind device is never promoted, and under usebackuproot nothing moves the root pointer.
4. It does not call the write-intent-log hooks, and it does not raise `dev->sb_generation`, since the generation is unchanged.

**btrfs_commit_gen_fence(fs_info)**:
```c
	for (int i = 0; i < 3; i++) {
		const u64 fence = READ_ONCE(fs_info->gen_fence);

		if (btrfs_get_last_trans_committed(fs_info) >= fence)
			return 0;
		trans = btrfs_start_transaction(fs_info->tree_root, 0);
		if (IS_ERR(trans)) { ret = PTR_ERR(trans); goto err; }
		ret = btrfs_commit_transaction(trans);	/* an older running one commits below F; loop */
		if (ret) goto err;
	}
	ret = -EUCLEAN;
err:
	btrfs_err(fs_info, B3 ...);
	return ret;
```

**btrfs_report_behind_devices(fs_info)**:
- Called after btrfs_verify_dev_items (disk-io.c:3647).
- For each opened device with `generation < fs_devices->mount_sb_generation`, print B4 (one line per device). It only prints; there is no knob.

**8. Call sites**
- **open_ctree**, disk-io.c:3745-3752. Inside `if (!sb_rdonly(sb) || log_replay)`, after btrfs_wib_rw_mount succeeds: `ret = btrfs_set_gen_fence(fs_info); if (ret) goto fail_sysfs;`.
  - The RAID56 recovery stays the first thing that writes stripes; it joins no transaction.
  - The fence is installed before the first possible join, which is btrfs_zoned_reserve_data_reloc_bg at 3777.
- **The log replay's own transaction** takes F and commits it. The log root has already been read, and nothing commits before the replay; committing earlier would clear log_root and lose the fsync'd data.
- **Read-only mount with replay:** after btrfs_replay_log (3789), `if (sb_rdonly(sb)) { ret = btrfs_commit_gen_fence(fs_info); if (ret) goto fail_qgroup; }`. This is normally a no-op, because btrfs_replay_log already committed (disk-io.c:2072-2076).
- **btrfs_start_pre_rw_mount**, before btrfs_resume_balance_async (disk-io.c:3170): `ret = btrfs_commit_gen_fence(fs_info); if (ret) return ret;`.
  - open_ctree then close_ctree()s and fails the mount (3816-3820).
  - A remount returns the error and stays SB_RDONLY.
- **super.c btrfs_remount_rw**, after btrfs_wib_rw_mount and before btrfs_start_pre_rw_mount: `ret = btrfs_set_gen_fence(fs_info); if (ret) return ret;`.
- **btrfs_replay_log**, disk-io.c:2052:
  `check.transid = (btrfs_mount_reuses_generation() ? fs_info->generation : btrfs_get_last_trans_committed(fs_info)) + 1;`
  The log was written in transaction committed+1 in every case. This also fixes the latent zoned case where a join precedes the replay.
- **tree-log.c btrfs_sync_log**, at the first full-commit check (3336):
  `if (btrfs_need_log_full_commit(trans) || unlikely(btrfs_gen_fence_pending(fs_info)))`
  where `btrfs_gen_fence_pending()` is `btrfs_get_last_trans_committed() < READ_ONCE(gen_fence)`. No fsync can write a superblock at the old generation before the fence commit. This is belt and braces: userspace cannot reach it today.

**9. Test hook** `btrfs.commit_early_writeback` (transaction.c, CONFIG_BTRFS_DEBUG, bool, 0644).
- In btrfs_commit_transaction, just before update_super_roots (2547): `if (unlikely(...) && !btrfs_is_zoned(fs_info)) filemap_fdatawrite(fs_info->btree_inode->i_mapping);`.
- It exercises the in-memory super bump.
- The implementer must first check that btree writeback actually writes blocks of the committing transaction at this point. If it does not, the writeback arm is INCONCLUSIVE; say so in the README.
- Lockdep boot required, because tree_log_mutex and reloc_mutex are held at this point.

### 2.4 Knobs (disk-io.c unless noted; CONFIG_BTRFS_DEBUG; bool; 0644)

| Knob | Description / effect |
|---|---|
| `btrfs.mount_reuses_generation` (renamed from the proposal's `degraded_reuses_generation`, because the fence now covers every rw mount) | "A writable mount starts at the next generation and commits nothing at mount, as before (testing only: restores a known silent-loss defect)". No fence, no intent write, no mount commit, and the replay reads at `generation + 1`. The rest of the old behaviour follows: the log guard and join override are inert with `gen_fence == 0`. It prints the B7 marker. |
| `btrfs.gen_fence_degraded_only` | Fence only when something is absent or a device is ahead, as in the first proposal. Control for the addcrash and tgtcrash arms. |
| `btrfs.gen_fence_volatile` | No intent write, and recorded intents are ignored. Control for the chain1 arm. |
| `btrfs.commit_early_writeback` (transaction.c) | Test hook, §2.3 item 9. |

All four are read when the fence is installed: at open_ctree for a writable or replaying mount, and in btrfs_remount_rw. Control runs pass them on the kernel command line.

### 2.5 Alerts

- **B1: fence with devices absent** (btrfs_warn, once per mount or remount):
  `read-write mount with %llu device(s) absent (devid %s): this mount writes generation %llu next, skipping %llu-%llu, and commits once now, so that a superblock an interrupted commit left on an absent device can never be taken for this mount's`
  - Up to 4 devids are listed; the replace target shows as devid 0.
- **B1'** (nothing absent, `base > gen`):
  `a device of this filesystem carries generation %llu, above the %llu being mounted%s; continuing at generation %llu and committing once now`
  - `%s` = `" (rescue=usebackuproot)"` when set.
- **Healthy mount:** btrfs_debug only (`generation fence %llu`). There is no line at a normal mount.
- **B2: intent write failed** (btrfs_warn):
  `could not record generation fence %llu on devid %llu (%s): %pe; if this mount is interrupted before its first commit completes and this disk is the only one the next mount shares with it, an older superblock could be taken for current -- replace devid %llu`
- **B3: fence commit failed** (btrfs_err; the mount or remount fails):
  `cannot commit the generation fence %llu: %pe; read-write refused, because the next commit could reuse a generation an absent device may carry. To read, mount with -o ro%s%s; otherwise bring the absent device(s) back, or fix the error above, and mount again`
  - The first `%s` is `",degraded"` when something is absent.
  - The second `%s` is `",rescue=nologreplay"` when `btrfs_super_log_root != 0`. A plain ro mount replays and would fail the same way.
- **B4: device behind** (per opened device at mount):
  - More than one generation behind (btrfs_warn):
    `devid %llu (%s) is behind: superblock generation %llu, filesystem %llu; it missed commits while it was absent or failing, so its copies of anything written since are stale. Reads detect them and use another copy; run 'btrfs scrub start' to rewrite them`
  - Exactly one behind (btrfs_info):
    `devid %llu (%s) is one generation behind (%llu, filesystem %llu), which is usual after a crash during a commit; its copies of that commit may be stale, and reads detect them`
- **B5** (warn once per devid):
  `ignoring generation %llu seen on devid %llu: more than 2^32 above the mounted %llu (corrupt superblock?)`
- **B6**: the overflow error, §2.3.
- **B7**: the control marker, §2.3.
- **Existing lines** that become the loud replacement for silent loss: `parent transid verify failed ... wanted >=F found N+1`, followed by `read error corrected` or a rebuild.

### 2.6 On-disk impact

- There is no feature bit, and no new item, key or field semantics beyond the ones below.
- **Generation gaps.** Values in base+1..F-1 are never committed. Nothing requires consecutive generations:
  - the tree-checker and progs check bounds;
  - chunk, cache and uuid generations must be equal only within one commit;
  - log_root = committed + 1 still holds, because the fence commit comes before any log commit.
- **`__unused_log_root_transid` now carries a lower bound for the next writable session.**
  - It is `<= generation`, except inside the fence-commit window.
  - Old kernels ignore it and carry it forward through super_copy. progs prints it as deprecated.
  - Very old kernels may have left a small stale value there, which is harmless as a lower bound.
- Old kernels and progs mount, check and replay a fenced filesystem unchanged. A returning orphan holder loses the superblock pick even under an old kernel.

### 2.7 UML reproduction: `tools/testing/btrfs/uml/orphan_super.sh <kernel> [arm...]`

It reuses the boot() pattern of degraded_crash.sh through lib_xholes.sh (§5). Images are 1 GiB. disk0 goes first to mkfs over dm, so it is devid 1 = D, which wins a tie.

Profiles:
- r1 = raid1:raid1 x2 (deterministic: every chunk has a copy on D);
- r5 = raid5:raid1 x3. The script must confirm a METADATA stripe on devid 1 with `dump-tree -t chunk`, else INCONCLUSIVE.

Boots:
- **B1 orphan_prep** (all disks, dm):
  - mkfs; mount /dev/mapper/d0; write `base`;
  - for shape=block also `btrfs subvolume create sv`;
  - sync; umount. Host: G0 = `sbgen disk1`.
- **B2 orphan_commit** (all disks, dm):
  - Mount. On FIXED this mount commits its own fence F0 at once; G0 is recomputed from disk1 on the host after B2.
  - For replay and roreplay only: `fsync_file pre`.
  - Write `orph` with no fsync.
  - `dm_drop_writes_ranges i 65536 4096 67108864 4096` for i = 1..ndev-1 (not for nofault).
  - `sync`, then `echo o` at once.
  - Host: ORPHAN_OK when `sbgen disk0 == sbgen disk1 + 1` and the other disks equal disk1, else INCONCLUSIVE. For replay and roreplay, `sblog disk1 != 0`.
  - Always read generations **from the host images**: write_dev_supers fills the bdev page cache before submitting (disk-io.c:3960), so an in-guest buffered read lies.
- **B3 orphan_degraded** (host omits disk0; arm knobs on the command line). dmesg grep for B1/B3/B7, `start tree-log replay`, `parent transid verify failed`, `tree block corruption`, `invalid .*generation`. Shapes:
  - `log` (j=0): `-o degraded`; `fsync_file deg`; `echo o`. Host: disk1 log_root != 0. disk1 gen: CONTROL G0, FIXED G0+2.
  - `tie` (j=1): `fsync_file deg`; umount. CONTROL needs disk1 == G0+1, else loop `touch x; sync` until it does, or INCONCLUSIVE. FIXED needs disk1 >= G0+3.
  - `block` (j=2): deg; sync; `sv/y` 64 KiB; sync; umount.
  - `remount`: `-o degraded,ro`; `remount,rw` (the B1 line must appear at the remount); deg; umount.
  - `replay`: B2 left a log; the degraded mount replays it; pre's md5 must match; deg; umount.
  - `roreplay`: `-o degraded,ro` replays and commits. Check pre. Host: disk1 == G0+2 FIXED, == G0+1 CONTROL.
  - `nofault`: tie shape after a B2 without drops.
- **B4 orphan_return** (all disks, plain rw mount):
  - Log `btrfs device scan`.
  - For W in 0 and 1: `pid_parity W mount /dev/ubda $MNT`; `echo 3 > drop_caches`; then `pid_parity W md5sum $MNT/deg`, `pid_parity W md5sum $MNT/base`, `pid_parity W test -e $MNT/orph`, **each reader wrapped separately** (find_live_mirror uses `current->pid % num_stripes`, volumes.c:6668); umount.
  - Then mount, `btrfs scrub start -B`, parse corrected / uncorrectable / **super_errors separately**, repeat the parity reads, umount.
  - Host: `btrfs check --readonly`.
  - dmesg: `is behind`, `parent transid verify failed`, `first key mismatch`, `read error corrected`, `bad tree block`.
  - Classify LOST as "deg absent or wrong with no dmesg line naming deg's inode, extent or csum". Unrelated owner or transid repairs are allowed and recorded.

Arms and expectations (every row: CONTROL, FIXED, ISO-X2; plus UP):

| Arm | Profiles | CONTROL / UP | FIXED and ISO-X2 |
|---|---|---|---|
| log | r1, r5 | deg ABSENT in both parities, mount clean (deterministic: G0+1 > G0) | deg OK; the log replayed under a super >= G0+2; B1 at B3 |
| tie | r1, r5 | D picked: deg ABSENT and/or orph VISIBLE, no EIO, no `is behind` for devid 1 | deg OK both parities; orph never visible; `devid 1 ... is behind` (or the one-behind info); reads of D's stale copies repaired or logged `wanted >= G0+2 found <= G0+1`; scrub corrected > 0, uncorrectable 0; afterwards both parities clean and check rc 0 |
| block | r1, r5 | evidence arm: raid1_copies.py shows a same-gen divergent pair (disk0 vs disk1: same gen, owner, level, different md5); expected read shape: orph listed or deg wrong in one parity, scrub 0 errors | as tie |
| remount | r1 | as tie | as tie; B1 at the remount |
| replay | r1, r5 | pre OK; deg lost (TIE) | pre OK; no transid or tree-checker error at B3; deg OK |
| roreplay | r1 | disk1 == G0+1 | disk1 == G0+2; pre OK |
| nofault | r1 | deg OK | deg OK |
| reappear (P1) | r1 | B3: d0 mapped `error` (neither scanned nor opened); `-o degraded,ro`; `dm_heal 0`; `btrfs device scan /dev/mapper/d0` (expect `missing devid 1 re-appeared`); `remount,rw`; deg; umount. CONTROL: TIE, deg LOST | B1 counts devid 1 absent; base from scan_generation = G0+1; F = G0+3; deg OK |
| chain1 (P2; tolerance 1) | raid1:raid1 x3 and raid5:raid1 x3; B=devid1, A=devid2, C=devid3 | control = FIXED + `gen_fence_volatile=1`: mount 1 (A omitted, `sb_crash_after_devid=1`), so B gets F1 and C keeps G0 with no intent; mount 2 (B omitted) gives F2 = F1; j=0 and j=1 variants; B returns: LOST (j=0) or same-gen collision (j=1, raid1_copies.py) | host: C's `fence` field == F1 after mount 1; mount 2's B1 line shows F2 >= F1+2; B returns loses; transid repairs loud; deg OK |
| chain (tolerance 2) | raid1c3:raid1c3 x4 | none (the old kernel does not commit at mount) | B3a: omit disk0, `sb_crash_after_devid=2`, so disk1 is at F1 and disk2,3 at G0 with intent F1. B3b: omit disk0 and disk1; B1 says "2 device(s) absent ... skipping ..." with F2 > F1; deg; umount. B4: deg OK; `is behind` for devids 1 and 2 |
| addcrash (P3) | single on d0, add d1 | control = FIXED + `gen_fence_degraded_only=1`, and CONTROL: B2 `btrfs device add /dev/mapper/d1` with `sb_crash_after_devid=2`, so d1 is (N+1, devid 2) and d0 is N; B3 omits d1, plain rw mount, `fsync_file deg`, crash (j=0); B4 all: d1 wins, deg LOST or CONTROL-LOUD (B3's log blocks may overwrite the orphan lineage's blocks) | B3 commits F = N+2 at mount; B4: d0 wins; deg OK |
| tgtcrash (P3) | r1 + d2 target | B2 `btrfs replace start -B 2 /dev/mapper/d2` with `sb_crash_after_devid=0`, so the target is at N+1 with STARTED; B3 omits the target, plain rw mount, one commit at umount (j=1); B4: tie at N+1, devid 0 sorts first and wins; the session is lost | B3 fence N+2; the target loses; deg OK |
| overflow (P5) | r1 | fixed only: host `btrfs-sb-mod disk0.img generation =$((1<<40))`; B3 maps d0 `error` **after** `btrfs device scan /dev/mapper/d0`, so it is scanned but not openable; `-o degraded` | B5 line; fence sane (G0+2); mount OK; no wrap |
| writeback | r1 tie and replay shapes | fixed only, `commit_early_writeback=1` | mount OK, no `write time tree block corruption`, no `invalid root generation`. Developer-only negative, not committed: remove the super bump, and the mount must fail with `invalid root generation`. |
| log_full | raid5:raid5 degraded_log_full column | extend degraded_log_full.sh: after its arm, `mount -o degraded` rw, then `btrfs replace start`; record mount time and outcome in CONTROL and FIXED | expected: same outcome as CONTROL, plus the B1 line; a longer mount is recorded, not failed |

Rig additions for this script:
- `dm_drop_writes_ranges` and `pid_parity` (§5);
- `raid1_copies.py`: maps a logical address to each copy from `dump-tree -t chunk` and prints bytenr, gen, owner, level, first key and md5 per copy;
- `btrfs-sb-mod` built and installed into `$T/progs-install/bin` (setup.sh);
- the `sb_crash_after_devid` hook (Lane 0).

### 2.8 Model

Extend tools/testing/btrfs/models/policy/attack/c_model.py (Lane M, optional, not blocking):
- `--mount-commit-crash`: the fence super lands on any subset of present devices.
- `--gen-skip-absent`.
- `--fence-intent`: a durable record on present devices before the fence commit.
- Absent sets that swap but keep the same size.
- Run RAID6 and RAID1C3 with 5 devices.

Expected:
- skip 1 → silent chains;
- skip = absent without intent → the tolerance-1 chain stays silent;
- with intent → 0, except disjoint-set split brain (documented).

### 2.9 fstests

- btrfs/003 011 124 125 (degraded mounts, replace, a device that returns);
- 197 198 254 (stale or re-appearing devices);
- groups `-g replace`, `-g volume`, `-g balance`, `-g log` (replay transid change), `-g quick`;
- `-g zone` on null_blk zoned if available (zoned replay transid; intent write via the sb log).

Every rw mount now commits once: compare against the baseline and explain every difference.

### 2.10 Risks and residuals

1. **Every rw mount now writes each present device's primary super once more, and commits once** (decision 2).
   - A disk that cannot take the fence commit fails the mount (B3) instead of flipping read-only at the first commit. `-o ro` (with `rescue=nologreplay` when a log is pending) still reads.
   - Measure with degraded_log_full.
2. **In-memory super_copy bump.** During the fence transaction, super_copy reports F-1, a value never committed. All current readers are audited:
   - tree-checker (intended);
   - block-group.c:2695 and find_newest_super_backup (mount time, before any join);
   - validate_write_super (after update_super_roots);
   - scrub.c:4836 and super.c:2407 (on-disk vs last committed, equal after the commit).
   If the fence transaction aborts, the value stays in memory only.
3. **Residuals.**
   - (a) **Split brain on disjoint device sets**, for example 2-disk RAID1 mounted degraded alternately. No shared device carries the intent. This is a documented btrfs limitation.
   - (b) **A device k > 1 generations ahead** because write_all_supers tolerated super-write failures on the other devices during a live session. That is X-1 or beyond-tolerance territory; stage 1's failed state is the fix.
   - (c) **btrfs-progs writers** (`check --repair`, `rescue`) on a degraded fs still commit N+1. Progs follow-up.
   - (d) An **intent write that fails on a present device** (B2) re-opens the chain if that disk is the only one shared with the next mount.
   - (e) **Zoned** intent writes go through the sb log. They are untested unless the zone group runs.
4. **After D returns**, nodatasum data on D's RAID1 mirror is silently stale. That is generic CUR-1, not X-2, and is reported separately. On RAID5/6 D's stale data columns go through the branch's readd path.
5. **Replay ordering** relies on nothing committing before btrfs_replay_log. That holds today; reading at committed+1 removes the dependence on joins.
6. **Stage 1:** T-missing's witness bound is `gen_fence - 1`, through an accessor, not last committed.

---

## 3. X-3: the disk a replace took out comes back as its devid

### 3.1 Confirmed

**Status: CONFIRMED by code reading in both trees. Not yet run.**
1. At finishing, T takes S's devid D and S's uuid (dev-replace.c:1015-1019). S gets devid 0 and T's uuid. The dev item and every chunk stripe keep (D, U_S), and btrfs_update_device never writes uuid or generation (volumes.c:3170-3207). So S and T are identical on disk except for the superblock generation.
2. The scratch of S is best effort:
   - it runs only when S is WRITEABLE (dev-replace.c:1058-1059);
   - it returns silently if the read fails (volumes.c:2257-2259);
   - it only warns on a write failure (2265-2267);
   - there is no flush.
3. Acceptance:
   - M1: the target's in-memory generation is 0 (dev-replace.c:297), so after an unmount a scan of S passes the tie-break at volumes.c:902, and S is taken as "missing devid re-appeared" (943).
   - M2: T is absent at the next boot, and S is the only candidate.
   - The degraded path clears MISSING (959-962).
4. read_one_dev checks the dev item generation only for seeds (volumes.c:7966-7970).
5. Upstream is identical (up:296, up:987-991, up:1030-1031, up:759/901/942, up:2086, up:7916).

### 3.2 Disputed points, checked in code

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| Q1 | The residual window lasts as long as the scratch of a failing disk. | **Confirmed** | After the swap, device_list_mutex is released at dev-replace.c:1053. Then come sysfs (1056-1057) and the scratch (1058-1059), and only then the next transaction (1062). `item_needs_writeback = 1` was already set at 958, so any commit in between persists FINISHED with T as devid D and no fence. **Fix accepted: hold handle H across the swap.** Feasibility checked: btrfs_reserve_chunk_metadata takes chunk_mutex itself (block-group.c:4619-4621), so the dev-item update must come **after** the unlock, still inside H. The order "handle, then dev_replace rwsem" already exists (dev-replace.c:707-710). btrfs_wib_replace_end takes only a spinlock (raid56-wib.c:5465-5528). A previous transaction that is past UNBLOCKED already wrote STARTED in commit_cowonly_roots, so H's transaction is the first that can persist FINISHED. |
| Q2 | The mount-time detach can wait on readahead with no bound. | **Confirmed** | btrfs_read_chunk_tree starts readahead_tree_node_children on the first slot (volumes.c:8188-8191), before any read_one_dev. Every other mount-time read before that point is synchronous, and the write-intent log loads later (disk-io.c:3612). **Fix accepted: gate the readahead to non-DEV_ITEM keys and drop the wait.** |
| Q4 | Single-device filesystems stay exposed even with T attached. | **Confirmed** | btrfs_skip_registration skips num_devices == 1 (volumes.c:1435-1437), and btrfs_close_devices frees a single-device fs_devices. KNOWN_GAP. |
| Q5 | Reuse of the same dev_t skips the scan check. | **Confirmed** | The check sits inside `!device->name \|\| device->devt != path_devt` (volumes.c:873). **Add the open-time check.** |
| Q3 | Laundering by progs or an old kernel. | Accepted as a documented gap | progs' read_one_dev ignores the dev item generation (progs only prints it, print-tree.c:387). zero-log's write_all_supers writes the current generation to S. |
| Q7-Q9 | Escape hatch, DUP system chunk hint, zoned reset count | Accepted | See 3.3. |
| Q10 | The knob must restore the old ordering exactly. | Accepted | Read once at entry. |
| - | A **pre-existing upstream quirk** I found while checking Q1. | Not fixed here; follow-up §7 | A transaction commit that is already waiting on device_list_mutex in write_all_supers when finishing takes it resumes after the swap. It then writes T's superblock with devid D and U_S (write_all_supers reads dev->devid and uuid at write time, disk-io.c:4262-4273) under a tree that still says STARTED. Inferred from code, not run. |

### 3.3 Final fix, function by function

**Principle.** Keep one u64 fence F(D) per devid, stored in the chunk tree in the dev item's existing `generation` field and only ever raised with `max()`. A disk that claims devid D with a superblock generation below F(D) is a disk a replace took out.

**Why `F = H->transid` is safe:**
- S leaves the device list inside the swap's device_list_mutex section, while H's transaction is RUNNING and held.
- Every superblock is written under that mutex, and log commits reuse the last committed generation.
- So every superblock S ever got is below `H->transid`.
- T is on the list for H's commit, so T's superblock is at least F once H commits.

The fence is checked in three places:
- scan, from memory;
- open, from memory;
- mount, in read_one_dev, which is authoritative.

**1. volumes.h / dev-replace.h**
- `struct btrfs_device`: `u64 fence_generation;`: a disk claiming this devid with sb generation below this is one a replace took out; 0 means none. Protected by uuid_mutex at mount and device_list_mutex after.
- Prototypes:
  - `int btrfs_fence_replaced_devid(struct btrfs_trans_handle *, struct btrfs_device *, u64 fence);`
  - `bool btrfs_replace_accepts_old_disk(void);` (static inline false without DEBUG)
- `btrfs_scratch_superblocks()` returns `int`: the number of copies not wiped.

**2. volumes.c: knob and predicate**
```c
static bool btrfs_is_old_disk(const struct btrfs_device *dev, u64 sb_gen)
{
	const u64 fence = READ_ONCE(dev->fence_generation);

	return fence && sb_gen < fence && !btrfs_replace_accepts_old_disk();
}
```

**3. volumes.c: `btrfs_fence_replaced_devid()`**, next to btrfs_update_device (3170):
1. `btrfs_reserve_chunk_metadata(trans, false)`. It takes chunk_mutex, so the caller must not hold it.
2. `btrfs_search_slot(trans, chunk_root, {DEV_ITEMS_OBJECTID, DEV_ITEM_KEY, device->devid}, path, 0, 1)`. `> 0` means `-ENOENT`.
3. If `btrfs_device_generation(leaf, di) < fence`, then `btrfs_set_device_generation(leaf, di, fence)`. The leaf is already COWed and dirty, as in btrfs_update_device.
4. `btrfs_trans_release_chunk_metadata(trans)`.
5. The test knob `replace_fence_fail` makes it return -EIO without writing.

This function never goes through btrfs_update_device, and the field stays untouched everywhere else.

**4. dev-replace.c: `btrfs_dev_replace_finishing()`** (loop 928-946, swap 951-1053, tail 1055-1077)
```c
	const bool legacy = btrfs_replace_accepts_old_disk();	/* read ONCE */
	struct btrfs_trans_handle *fence_trans = NULL;
	u64 fence = 0; int fence_ret = 0, unwiped = 0, crash = replace_crash_point_take();

	while (1) {
		trans = btrfs_start_transaction(root, 0);	/* unchanged */
		...
		ret = btrfs_commit_transaction(trans);
		WARN_ON(ret);
		if (!legacy) {
			/* H: the transaction that first persists FINISHED carries the fence. */
			fence_trans = btrfs_start_transaction(root, 0);
			if (IS_ERR(fence_trans)) { unlock lock_finishing_cancel_unmount; return PTR_ERR(fence_trans); }
		}
		mutex_lock(&fs_devices->device_list_mutex);
		mutex_lock(&fs_info->chunk_mutex);
		if (!list_empty(&src_device->post_commit_list)) {
			mutex_unlock(&fs_devices->device_list_mutex);
			mutex_unlock(&fs_info->chunk_mutex);
			if (fence_trans) { btrfs_end_transaction(fence_trans); fence_trans = NULL; }
		} else {
			break;
		}
	}
	...
error:	/* after the existing unlocks */
	if (fence_trans) btrfs_end_transaction(fence_trans);
	...
	/* swap block unchanged through up_write + btrfs_rm_dev_replace_blocked */
	btrfs_note_absent_super(fs_devices, src_device->sb_generation,
				tgt_device->devid, "replaced, not committed");	/* X-1 */
	btrfs_rm_dev_replace_remove_srcdev(src_device);
	btrfs_rm_dev_replace_unblocked(fs_info);
	atomic_inc(&tgt_device->dev_stats_ccnt);
	mutex_unlock(&fs_info->chunk_mutex);
	mutex_unlock(&fs_devices->device_list_mutex);

	if (!legacy) {
		fence = fence_trans->transid;
		if (crash == 1)
			panic("btrfs: replace crash injection 1: devid %llu swapped, fence %llu not committed",
			      tgt_device->devid, fence);
		fence_ret = btrfs_fence_replaced_devid(fence_trans, tgt_device, fence);
		btrfs_remove_dev_stat_item(fence_trans, BTRFS_DEV_REPLACE_DEVID);
		ret = btrfs_commit_transaction(fence_trans);
		replace_fence_done(fs_info, src_device, tgt_device, fence, fence_ret, ret);
		if (crash == 2)
			panic("btrfs: replace crash injection 2: fence %llu committed, old disk not wiped", fence);
	}
	btrfs_sysfs_remove_device(src_device);
	btrfs_sysfs_update_devid(tgt_device);
	if (test_bit(BTRFS_DEV_STATE_WRITEABLE, &src_device->dev_state))
		unwiped = btrfs_scratch_superblocks(fs_info, src_device);
	else if (src_device->bdev)
		unwiped = BTRFS_SUPER_MIRROR_MAX;	/* present but read-only: keeps its sb */
	if (legacy) {
		/* upstream verbatim: "write back the superblocks" transaction */
		trans = btrfs_start_transaction(root, 0);
		if (!IS_ERR(trans)) {
			btrfs_remove_dev_stat_item(trans, BTRFS_DEV_REPLACE_DEVID);
			btrfs_commit_transaction(trans);
		}
	} else {
		replace_wipe_report(fs_info, src_device, tgt_device, fence, unwiped);
	}
	mutex_unlock(&dev_replace->lock_finishing_cancel_unmount);
	...unchanged tail...
```
- `replace_fence_done()` behaviour:
  - When the commit (`ret`) **succeeded**, it sets `tgt->fence_generation = max(tgt->fence_generation, fence)` under device_list_mutex, **even if the item update failed**. The swap is then durable, so refusing S in this boot is correct. This refines the attack, which dropped the in-memory fence on any error.
  - When the commit failed, it sets no in-memory fence. The pre-swap state is still on disk.
  - On `fence_ret || ret` it prints C7. Otherwise it prints C4.
- `replace_wipe_report()` prints C5 when `!src->bdev`, and C6 when `unwiped`.
- All of this runs before btrfs_rm_dev_replace_free_srcdev, so the name is still valid.
- **Lockdep fallback.** If lockdep or fstests object to H being held across `down_write(rwsem)` and the bio drain, keep the fence item in a transaction started right after the unlock and committed before the sysfs update and the scratch. Document the residual as "a commit already in progress at the swap".

**5. volumes.c: `device_list_add()`**, at the top of the `else if (!device->name || device->devt != path_devt)` branch (volumes.c:873):
```c
		if (btrfs_is_old_disk(device, found_transid)) {
			mutex_unlock(&fs_devices->device_list_mutex);
			btrfs_err(NULL, C1 ...);
			return ERR_PTR(-EEXIST);
		}
```
It is inside that branch on purpose: a rescan of the disk already bound to the struct (T itself) is never refused.

**6. volumes.c: `btrfs_open_one_device()`**, after the uuid match (686-689):
`if (btrfs_is_old_disk(device, btrfs_super_generation(disk_super))) { btrfs_err(NULL, C2 ...); goto error_free_page; }`
This covers path or dev_t reuse in the same boot. The devid is then simply missing.

**7. volumes.c: `btrfs_read_chunk_tree()`** (8188-8191): start readahead only once `found_key.type != BTRFS_DEV_ITEM_KEY`. Dev items (objectid 1) sort before every chunk item, so no readahead bio can be in flight while read_one_dev runs.

**8. volumes.c: `read_one_dev()`** (7885ff)
- Read `const u64 fence = btrfs_device_generation(leaf, dev_item);`.
- Apply it only when `fs_devices == fs_info->fs_devices` (non-seed item; seeds keep their equality check at 7966-7970).
- Found device, at the top of the `else` branch, before `if (!device->bdev)`:
```c
		device->fence_generation = max(device->fence_generation, fence);
		if (device->bdev && btrfs_is_old_disk(device, device->generation)) {
			ret = btrfs_fence_old_disk(fs_info, device);	/* crit C3, then detach */
			if (ret)
				return ret;
		}
```
  The existing code then treats the devid as missing: without degraded it reports and returns -ENOENT; with degraded it sets MISSING.
- After add_missing_dev: `device->fence_generation = max(..., fence)`.
- `btrfs_fence_old_disk()` (static):
  - **No wait:** `ASSERT(percpu_counter_sum(&fs_info->dev_replace.bio_counter) == 0)` under CONFIG_BTRFS_ASSERT.
  - If `fs_devices->latest_dev == device` (impossible: a fence in the tree implies a super at or above F), return -EUCLEAN with a crit line rather than detach.
  - Otherwise detach exactly as __btrfs_free_extra_devids does (volumes.c:1063-1073):
    - if WRITEABLE: list_del_init(dev_alloc_list), clear WRITEABLE, rw_devices--;
    - `fs_bdev_file_release(bdev_file, bdev_file->private_data)`; bdev = bdev_file = NULL; open_devices--.
  - Then `rcu_assign_pointer(device->name, NULL)` + kfree_rcu_mightsleep, `devt = 0`, `generation = 0`. A later mount does not reopen S, and a scan of T takes the slot.
  - There is no DEV_REPLACING toggle, no 30 s loop, and no export of btrfs_rm_dev_replace_unblocked. This drops attack items 2 and 6 of the first proposal.

**9. volumes.c: scratch.**
- btrfs_scratch_superblock returns:
  - 0 when there is no copy (btrfs_read_disk_super -EINVAL: beyond the end, or not a btrfs super);
  - the read error for a read failure;
  - the sync_blockdev_range error for a write failure.
  The per-copy warning is kept.
- btrfs_scratch_superblocks returns the count of copies not wiped. It also counts a failed `btrfs_reset_sb_log_zones` on zoned devices.
- The callers at 2473 and 2594 ignore the return value.

**10. disk-io.c: `open_ctree()`**, where the chunk root or chunk tree read fails (3577-3590): append hint C8 for every opened device whose `generation < latest_dev->generation`. It is loud and changes no behaviour.

Deliberately not changed:
- tgt->generation at finishing (the generic tie-break stays fooled by generation 0 for non-replace stale copies);
- T's superblock dev_item.generation;
- any on-disk layout.

### 3.4 Knobs (CONFIG_BTRFS_DEBUG, 0644)

- `btrfs.replace_accepts_old_disk` (bool, volumes.c).
  - Description: "Let a disk that a device replace took out be taken for its devid again, and record no fence at the replace (testing only: restores a known data-loss defect)".
  - At 1:
    - finishing runs upstream's sequence verbatim (read once at entry);
    - the scan, open and mount checks are off (read at each use).
  - It supersedes stage 1's `raid56_wf_no_tombstone` for the replace case.
- Test only:
  - `btrfs.replace_crash_point` (int, dev-replace.c, xchg to 0): 1 = after the swap, before H commits; 2 = after H commits, before the scratch.
  - `btrfs.replace_fence_fail` (bool): btrfs_fence_replaced_devid fails with -EIO.

### 3.5 Alerts (printed only when the knob is 0)

- **C1: scan refusal** (btrfs_err NULL, -EEXIST):
  `device %s (%d:%d) is an old disk of fsid %pU: it claims devid %llu at generation %llu, below %llu where a replace put in the disk that is devid %llu now; not using it. Disconnect it; wipe it (wipefs -a %s) only after the filesystem is healthy without it. If this filesystem was restored from a copy older than generation %llu, run 'btrfs device scan --forget' first. Scanned by %s (%d)`
- **C2: open refusal**: the same text, with "not opening it" and without "scanned by".
- **C3: mount-time** (btrfs_crit):
  `devid %llu at %s is an old disk: superblock generation %llu, below %llu where a replace put in the disk that is devid %llu now; not using it, so devid %llu is missing%s. Connect the disk that replaced it and disconnect this one. While this disk is connected and its replacement is not, do not run 'btrfs check --repair', 'btrfs rescue' or btrfstune, and do not mount with an older kernel: any of those makes this disk look current. Wipe it only after the filesystem is healthy without it`
  - `%s` = `" and the mount needs -o degraded"` when not degraded.
  - The existing `devid %llu uuid %pU is missing` line follows.
- **C4** (btrfs_info at the end of a replace):
  `devid %llu: a disk claiming devid %llu with superblock generation below %llu is refused from now on`
- **C5** (btrfs_warn, the source was missing):
  `devid %llu: the disk the replace took out was missing, so its superblock still says devid %llu of this filesystem; it is refused if it comes back (generation below %llu). Wipe it (wipefs -a) before using it for anything else`
- **C6** (btrfs_warn, the scratch failed or the source is read-only):
  `devid %llu: could not wipe %d superblock copies of %s, the disk the replace took out; it still says devid %llu of this filesystem and is refused if it comes back (generation below %llu). Wipe it (wipefs -a %s) before using it for anything else; mounted on its own it shows this filesystem as of generation %llu`
  The per-copy `error clearing superblock number N` lines are kept.
- **C7** (btrfs_err, the fence could not be recorded):
  `devid %llu: could not record that the disk the replace took out (%s) is old: %pe; if that disk is connected after a reboot while %s is not, it is taken for devid %llu and its old data is read -- wipe it (wipefs -a) before connecting it again`
- **C8** (btrfs_err hint on a chunk root or chunk tree failure):
  `devid %llu at %s has superblock generation %llu, below the %llu being mounted; if it is a disk a replace took out, disconnect it and connect its replacement`
- **Test panics:** `btrfs: replace crash injection 1/2 ...`

There is no raid56_health event: X-3 applies to every profile. When task #93's filesystem-wide alert exists, C3 and C7 should raise it.

### 3.6 On-disk impact

- There is no new item, key, feature bit or superblock field.
- The dev item `generation` of a replaced devid holds the transid of the replace's finishing transaction, raised with `max()`.
- Compatibility, checked by reading:
  - **Kernel.** Non-seed items are always written with 0 (volumes.c:2087) and read nowhere. The only reader is the seed equality check (7966-7970), and the only other writer is btrfs_finish_sprout for seeds (2857).
  - **Tree-checker.** It skips the field (tree-checker.c:1201-1204).
  - **btrfs-progs.** It only prints the field (print-tree.c:387). It writes 0 only for new items (volumes.c:1170, mkfs, convert). `btrfs rescue chunk-recover` resets it to 0 (rescue-chunk-recover.c:1227): the protection is lost, but there is no false positive.
  - **Old kernels** ignore and preserve it, with no protection and nothing broken.

### 3.7 UML reproduction: `tools/testing/btrfs/uml/replace_ghost.sh <kernel> [arm...]`

Setup:
- MODE `replace_ghost` with ARM and PHASE; NDEV=5; 1 GiB ubd images; dm_setup gives d0..d4.
- `mkfs.btrfs -K -f -d raid5 -m raid1 d0 d1 d2 d3`; `btrfs device scan --forget`; `dm_scan_only 0 1 2 3`; mount d0.
- S = d2 (devid 3), T = d4.
- X1 = 32 MiB of `A`, then sync.

Fault S per arm:
- drop: `dm_drop_writes 2`;
- ewrite: `dm_error_writes 2`;
- sbonly: `dm_error_writes_range 2 65536 4096`.

Then:
1. `btrfs replace start -B -f 3 /dev/mapper/d4 $MNT`. Log rc and status. rc != 0 makes the arm **INCONCLUSIVE**; strict refusal may latch S or cancel the replace.
2. `remount,nodatasum`; X2 = 48 MiB of `B` in a new file. If the branch forces COW, use `allow_nodatacow` and `chattr +C`.
3. `remount,datasum`; X3 = 48 MiB of `C`.
4. sync; record the md5s; umount.
5. Parse N from C4.

Identity proof, after umount:
- `sb_ident /dev/ubdc` (raw S) and `/dev/ubde` (raw T), plus `sb_ident /dev/mapper/d2` right before the trigger. The kernel scans the dm path, whose page cache is separate from the raw device's.
- With d2 `dm_detach`ed: `dump-tree -t chunk /dev/mapper/d0`, and read DEV_ITEM 3's `generation`: FIXED = N, CONTROL = 0. Then `dm_heal 2`.
- Expect: S magic valid; devid and uuid equal on S and T; `gen(S) < N <= gen(T)`.

The script must never call:
- do_mount (it runs zero-log, init-final3.sh:85);
- dm_scan after the replace (its `--forget` hides M1);
- a bare `btrfs device scan`.

Phases:
- **A (M1, same boot):**
  - `btrfs device scan /dev/mapper/d2` only (log rc);
  - `mount -o ro /dev/mapper/d0`, no degraded;
  - record: `fi show` (the path for devid 3), drop_caches, md5 of X1/X2/X3, rc of `cat X2`, and new warn/err/crit dmesg lines since a kmsg marker;
  - umount. Read-only keeps the phases independent.
- **B (M2 in one boot):**
  - `btrfs device scan --forget`; `dm_scan_only 0 1 2 3`;
  - `mount -o ro`, no degraded (log rc);
  - FIXED only: `mount -o ro,degraded`, check X1/X2/X3, umount, `dm_scan_only 4`, `mount -o ro`, and T must be used.
- **C (CONTROL only, lock-in):**
  - rw mount via S, write 1 MiB, sync, umount;
  - `--forget`; register all 5;
  - expect `already registered with a higher generation` for d4; X2 still wrong.

| Arm | CONTROL | FIXED |
|---|---|---|
| drop / ewrite / sbonly (A, B, C) | ghost=1: `fi show` lists d2 as devid 3, the mount needed no degraded, X2 md5 wrong with `cat` rc 0, no fence lines | A: C1 on the scan (rc != 0); T used; fi show never lists d2. B: no-degraded mount fails with C3 naming devid 3 and d2; degraded reads X1, X2, X3 correct. ewrite/sbonly also show C6. |
| missing | X1; umount; `dm_detach 2`; `--forget`; scan d0 d1 d3; `-o degraded`; replace (rebuild); X2/X3; `dm_heal 2`; scan d2 while mounted gives `duplicate device`; then phase A gives ghost | C5 at the replace; scan d2 while mounted gives C1; phase A refused |
| degraded | drop setup; umount; `dm_detach 4`; `dm_heal 2`; `--forget`; scan d0 d1 d3; `-o ro,degraded`; scan d2 while mounted gives `missing devid 3 re-appeared`; umount; `-o ro` without rescan mounts with S; X2 wrong | scan d2 while mounted gives C1; `-o ro` fails (devid 3 missing); with `-o degraded` X2 is correct |
| absent (2 boots) | boot 2 omits ubd4; dm_setup gives d0..d3; register all; `-o ro` without degraded succeeds; fi show lists d2; X2 wrong | the mount fails with C3; `-o ro,degraded` gives X1/X2/X3 OK; scan d2 while mounted gives C1 |
| healthy (fixed only) | - | S unfaulted: S's magic wiped, C4 present, `gen(T) >= N`; scan d4 after umount accepted; `--forget`, register all, rw mount: no fence or crit lines. Second replace 3 → /dev/mapper/d2 (already wiped): new fence N2 > N, and dump-tree shows N2. `btrfs check --readonly` rc 0. |
| crash1 (2 boots) | - | Boot 1: drop setup, `replace_crash_point=1`, replace panics. Boot 2: every device, `dm_drop_writes 2` again. Record T's raw super devid: 0 is the pre-swap state; anything else logs RESIDUAL_WINDOW (the §3.2 upstream quirk) and the arm is INCONCLUSIVE. The replace resumes; poll `btrfs replace status -1`; C4 appears; X2 nodatasum; umount; phase A: refused, X2 correct. CONTROL: ghost. |
| crash2 (2 boots, **healthy S**, no dm fault) | S left unscratched deterministically; boot 2 omits ubd4 and mounts S (acceptance) | boot 2 fails with C3, which proves the fence was durable in H |
| devt_reuse (Q5) | reload d4's table onto S's ubd after umount; `dm_scan_only 4`; mount: S used as devid 3 | C2 at open; devid 3 missing; degraded read OK |
| split | - | fence recorded with the knob at 0, then the knob set to 1 before the trigger: ghost. This shows the three checks are the only barrier. |
| fence_fail | - | `replace_fence_fail=1`: C7 at the replace; dev item generation 0. Phase A refused (the in-memory fence is set because H committed). Phase B (after `--forget`): ghost, known and loud at replace time. |
| laundering | KNOWN_GAP in both arms | T absent; `btrfs rescue zero-log /dev/mapper/d2`; mount: S accepted in both arms. Records the documented gap. |
| single | KNOWN_GAP | single-device replace with the old disk failing writes: the old disk mounts silently as an older fs. The C6 text is the only signal. |

Rig additions:
- `dm_scan_only`, `sb_ident`, kmsg markers (§5);
- a two-boot runner with per-boot ubd lists (lib_xholes.sh);
- the `replace_crash_point` and `replace_fence_fail` knobs;
- a KASAN kernel run of the whole script once, for the detach path.

### 3.8 fstests

- The replace group, with focus on:
  - btrfs/027 and 286: replace of a missing device;
  - btrfs/124 and 125: a device that reappears **without** being replaced must still be accepted;
  - btrfs/011, 064, 065, 069, 070, 071 (replace under load).
- The seed group (btrfs/161 162 163 216 225 238 248 249 298 315 323), with focus on 163 and 225: replace on a sprout, seed dev items.
- The volume group, including 197, 198, 219, 254 (stale-device lists).
- `btrfs check --readonly` after the healthy arm.
- Use group membership as authoritative if numbers moved.

### 3.9 Risks and residuals

1. **Conservative false positive.**
   - T itself is refused if it missed every superblock from H onward, because its writes kept failing from the end of the replace. It is then missing, loudly, and the mount needs `-o degraded`.
   - C3 says "connect the disk that replaced it". Such a T is stale anyway.
2. **H held across the swap.**
   - H is held across `down_write(rwsem)`, the bio drain and `btrfs_wib_replace_end`.
   - The order "handle, then rwsem" already exists, and the drain was already done under device_list_mutex, which blocks write_all_supers.
   - Verify with a PROVE_LOCKING boot and the replace fstests. The fallback is in 3.3 item 4.
3. **Documented gaps** (README and COMPARISON.md §7):
   - laundering by progs writes or old kernels, followed by M3 lock-in;
   - single-device filesystems, even with T attached;
   - replaces done before this code;
   - chunk-recover resetting the field;
   - the residual window if the lockdep fallback is used.
4. **Boundary behaviour change.**
   - `mount /dev/<old>` and `btrfs device scan <old>` fail with EEXIST even while T is registered. This matches the existing tie-break.
   - A by-uuid link that resolves to the old disk makes the mount fail loudly instead of silently using it.
5. **Seed devices** are excluded and keep their equality check.
6. **The readahead gate** delays chunk-tree readahead until the first chunk item. The cost is negligible, since dev items are few.

---

## 4. Interactions

- **X-1 × X-2.**
  - X-2's fence commit at every writable mount makes X-1's mount-time "missing member" and "absent target" cases fire only in ISO-X1 runs. They stay as defence in depth and for runtime detach sites.
  - X-1's degr0 and repl_absent arms must be scored per arm scheme (FIXED shows B1, ISO-X1 shows the X-1 warning).
  - Both paths return BTRFS_LOG_FORCE_COMMIT from btrfs_sync_log.
  - X-2 reads `fs_devices->mount_sb_generation`, which X-1's init sets. Lane 0 sets it first so that lane B does not depend on lane A.
- **X-1 × X-3.**
  - The X-1 detach note in finishing moves into Lane C's reworked function.
  - Log commits during H's transaction are forced (the source's sb_generation equals the last committed generation). They join H's transaction and commit it after H ends. That is correct and cannot deadlock: btrfs_commit_transaction handles a second committer.
- **X-2 × X-3.**
  - A fenced old disk is detached in read_one_dev, before btrfs_set_gen_fence. It counts as absent, which only raises the skip.
  - X-3's fence value is `H->transid`, which may sit after a gap. That is fine.
  - The intent write never touches the old disk.
- **Stage 1 and 2** (approved to build, default off; land after these fixes):
  - reuse `dev->sb_generation` for erratum 6 / F1;
  - add a "failed" reason to the X-1 classifier;
  - T-missing uses `gen_fence - 1` as sb_bound;
  - X-3's fence replaces the X4 tombstone for the replace case, and `raid56_wf_no_tombstone` is dropped there;
  - FAILED entries keep their own sb_bound for a failed disk that has not been replaced.
- **COMPARISON.md §7 corrections (Lane D):**
  - Row 9 (X-3): "key devices by devid plus uuid" is wrong, because the uuid is swapped onto T. Replace it with "per-devid generation fence in the dev item".
  - Row 2 (X-1): the fix is "force a commit when a log commit's primary super missed any device that may hold its generation, including absent holders". The generation skip and mount commit belong to row 10.
  - Row 10 (X-2): "fence every writable mount at max(1, absent) generations above every generation any device may carry, recorded on every present device first, and commit it at mount".

---

## 5. Shared rig additions (Lane R0)

**init-final3.sh helpers** (next to dm_bad_sectors, lines 334-363):
- `dm_fail_primary_super idx`:
  - is `dm_bad_sectors idx 65536`;
  - the dm error target has `num_flush_bios = 0`, so barriers still succeed;
  - reads of that block fail during the boot, so read supers at the next boot on the raw ubd.
- `dm_drop_writes_ranges idx off len [off len ...]`:
  - builds the table like dm_bad_sectors, with `$s $n flakey $d $s 0 1000 1 drop_writes` segments and linear elsewhere;
  - offsets are sorted ascending;
  - `SB_RANGES="65536 4096 67108864 4096"`.
- `dm_delay_supers idx ms`:
  - a `delay` target over [64 KiB, 4 KiB] and [64 MiB, 4 KiB], linear elsewhere;
  - logs `DM_DELAY_MISSING` and returns 1 if the target is absent; the arm is then INCONCLUSIVE.
- `dm_scan_only idx...`: `btrfs device scan /dev/mapper/d$i` per index, with no `--forget`, logging each rc.
- `sb_ident dev`:
  - runs `blockdev --flushbufs dev`, then `btrfs inspect-internal dump-super`;
  - logs `SB dev=.. magic_ok=.. devid=.. uuid=.. gen=.. log_root=.. fence=..`, where `fence` is the `log_root_transid (deprecated)` line.
- `fsync_file name MiB [pattern]`: `dd conv=fsync`; the md5 goes into the manifest only when rc is 0; logs `FSYNC_OK name` or `FSYNC_FAIL name rc`.
- `pid_parity W cmd...`:
  - loops `sh -c '[ $(($$%2)) -eq W ] || exit 213; exec "$@"' _ cmd...` until rc != 213, then returns cmd's rc;
  - each reader is wrapped separately.
- `kmark tag` / `kmsg_since tag regex`: writes `XHMARK tag` to /dev/kmsg, then greps dmesg after the last marker.

**New host library `tools/testing/btrfs/uml/lib_xholes.sh`**, sourced by the three new scripts:
- `xh_boot tag ndev mode omit opts [args...]`:
  - degraded_crash.sh:57-73 generalised, with explicit `ubdN=` so names stay stable when disks are omitted;
  - copies init-final3.sh atomically to `$T/umltest/init-xh.sh`, like the other scripts.
- Host readers on image files:
  - `xh_sb img field`, using `$T/progs-install/bin/btrfs inspect-internal dump-super`;
  - wrappers `sbgen`, `sblog`, `sbfence`, `sbdevid`.
- `xh_args ARM` echoes the kernel arguments for CONTROL / FIXED / ISO-X1 / ISO-X2 / ISO-X3 (§0.3), plus per-arm extras.
- `xh_verdict`: the §0.3 rules.
  - The self-test noise filter is `(efault)`.
  - KERNEL_SPLAT or WATCHDOG means FAIL.

**setup.sh**:
- add `-e DM_DELAY`;
- build and install `btrfs-sb-mod` from the progs tree into `$T/progs-install/bin`;
- document a fast test kernel without `BTRFS_FS_RUN_SANITY_TESTS` (`uml-xh`), plus the normal one for selftests. uml-s0w spends about 25 s per boot on self-tests.

**README.md**:
- one row per new script, one row per knob;
- the arm scheme (§0.3);
- Known gaps (§1.10, §2.10, §3.9), including no udev in the guest (scan orders are emulated), zoned untested, and nobarrier not covered.

**regress.sh**: add the FIXED arms of the three scripts.

**residual-exposures.txt**: unchanged. It tracks the RAID56 model sweep, not these.

---

## 6. Implementation plan

### 6.1 Lanes and files

| Lane | Content | Files (functions) |
|---|---|---|
| **L0 scaffold** (behaviour-neutral, lands first) | Struct fields for all three holes; accessor; uapi comment; write_all_supers signature with both callers passing NULL; `btrfs_note_absent_super()` body; `mount_sb_generation` assignment at disk-io.c:3604; the `sb_crash_after_devid` test hook | volumes.h, fs.h, accessors.h, include/uapi/linux/btrfs_tree.h (comment), disk-io.h, disk-io.c (write_all_supers submit loop hook + signature; open_ctree 3604), transaction.c:2607, tree-log.c:3552 (NULL), volumes.c (new function only) |
| **R0 rig scaffold** | §5 helpers, lib_xholes.sh, setup.sh, README stub | tools/testing/btrfs/uml/{init-final3.sh (helpers section only), lib_xholes.sh, setup.sh, README.md} |
| **LA: X-1** | §1.3 items 3-5; knobs; the three volumes.c note calls | tree-log.c (btrfs_sync_log tail 3550-3574, knob block), disk-io.c (write_all_supers body; open_ctree after 3604 and after 3674), volumes.c (btrfs_rm_device 2439, btrfs_destroy_dev_replace_tgtdev 2590, btrfs_init_new_device 3132) |
| **LB: X-2** | §2.3 | transaction.c (join_transaction 393; commit hook near 2547), disk-io.c (fence functions, csum helper, knobs; open_ctree 3647, 3745, 3789; btrfs_replay_log 2052; btrfs_start_pre_rw_mount 3170), super.c (btrfs_remount_rw), volumes.c (device_list_add end, btrfs_open_one_device after the uuid match, btrfs_close_one_device), tree-log.c (3336 guard) |
| **LC: X-3** | §3.3, plus the X-1 note call in finishing | dev-replace.c (btrfs_dev_replace_finishing, crash and fail knobs), volumes.c (btrfs_fence_replaced_devid, knob, btrfs_is_old_disk, read_one_dev, btrfs_fence_old_disk, btrfs_read_chunk_tree, device_list_add branch top, btrfs_open_one_device after the uuid match, scratch), disk-io.c (open_ctree 3577-3590 hint) |
| **RA / RB / RC** | log_super_tie.sh, orphan_super.sh (+ raid1_copies.py, degraded_log_full.sh step), replace_ghost.sh, and their MODE blocks in init-final3.sh, each appended in its own delimited section | tools/testing/btrfs/uml/* |
| **LM** (optional) | c_model.py flags (§2.8) | tools/testing/btrfs/models/policy/attack/c_model.py, README |
| **LD** | COMPARISON.md §7 rows 2, 9, 10; policy README note on off-list holders; design-synthesis cross-reference (stage 1 reuses sb_generation, gen_fence, X-3 fence) | tools/testing/btrfs/models/policy/COMPARISON.md, README.md; scratchpad design note |

Shared-function overlaps (merge by hand, in the order below):
- device_list_add: LB edits the end, LC the branch top.
- btrfs_open_one_device: LB and LC both edit after the uuid match. LC's check goes **before** LB's fence_rec read.
- open_ctree: LA, LB and LC touch distinct hunks.
- tree-log.c: LA edits the tail, LB the 3336 guard.
- write_all_supers: L0 adds the hook, LA the body.

### 6.2 Order and gates

1. **L0 + R0.**
   - Build with W=1, sparse and checkpatch, plus a `CONFIG_BTRFS_DEBUG=n` build.
   - Selftests (CONFIG_BTRFS_FS_RUN_SANITY_TESTS) pass.
   - Two existing arms (regress.sh quick set) are unchanged.
2. **Controls first (TDD).**
   - Write RA/RB/RC and run every CONTROL arm on **uml-up** and on the L0 kernel.
   - Each must show LOST or CONTROL-LOUD, or be marked KNOWN_GAP or INCONCLUSIVE with a reason.
   - Arms whose CONTROL result is inferred from code only:
     - X-1: rm, repl_absent;
     - X-2: every arm (nothing run yet);
     - X-3: every arm.
   - These must be confirmed here before any fix code lands.
3. **LA, LB, LC in parallel**, in separate worktrees from L0. Each lane:
   - runs its own FIXED and ISO arms and all its knobs' control arms;
   - runs a PROVE_LOCKING boot (LC: H across the swap; LB: commit hook);
   - runs selftests with default knobs and with each new knob set.
4. **Merge LA → LB → LC** into strict-wip.
   - LB rebases on LA for tree-log.c and open_ctree.
   - LC rebases on both for volumes.c and open_ctree.
   - After each merge: build, selftests, and the merged lanes' arms.
5. **Full verification on the merged kernel:**
   - (a) every arm of the three scripts in CONTROL / FIXED / ISO-Xn, on all listed profiles;
   - (b) the existing scripts, because every writable mount now commits and replace finishing changed: degraded_crash, degraded23, degraded_csum, degraded_fresh, degraded_log_full, readd_admit, readd_flush, flush_wedge, replace_abort, replace_marks_kept, replace_rmw_resident, replace_source_data, replace_target_fail, replace_torn_free, replace_whole_column, recover_interrupt, recover_scrub, replay_full, run3 (prepare, prepare_fsync), selftest.sh, regress.sh. Explain every difference from the last baseline. Look especially for scenarios that inject a device fault **before** mounting read-write, which now meet the fence commit at mount;
   - (c) fstests §1.9, §2.9 and §3.8 against the volume-s0e baseline;
   - (d) a KASAN kernel for replace_ghost.sh and orphan_super.sh;
   - (e) W=1, sparse, checkpatch, and a PROVE_LOCKING boot of the merged tree.
6. **LD docs, LM model.** Commit one change per hole, plus the scaffold and rig commits, in upstream-splittable form:
   - X-1 needs no on-disk change and is the easiest to upstream;
   - X-3 and X-2 each reuse an unused field and need discussion.

---

## 7. Follow-ups (separate tasks, not in this change)

1. **btrfs-progs `super-recover`:**
   - at equal generation, prefer a primary over mirrors;
   - refuse without `--force` when same-generation primaries disagree on log_root (X-1 D2).
2. **btrfs-progs `read_one_dev`:**
   - treat a non-seed device whose superblock generation is below its dev item generation as missing, and refuse write opens (X-3 laundering);
   - make offline writers on a degraded fs follow the X-2 fence rule.
3. **Replace resume after a degraded session with the target absent:** reset cursor_left to 0, with a knob. Otherwise nodatasum data behind the cursor is stale on the new disk (X-1 attack, finding 8).
4. **Upstream quirk (§3.2):** a commit that is waiting on device_list_mutex during replace finishing writes the target's new identity under a tree that says STARTED. Reproduce with a pause hook, then decide.
5. **Zoned:** resync the sb-zone write pointer after a failed superblock write (X-1 D7).
6. **Upstream ENOSPC** during `btrfs replace start`, followed by a crash, leaves an unmountable fs (research side finding).
7. **Optional read-only rescue option** that lets an X-3-fenced disk serve reads, with a crit line naming F, for the double-failure case.
