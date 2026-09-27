# fstests: upstream 7.3-rc2 vs s0e vs strict (am / am2), RAID5/6 and volume lists

Status: **paused on request, incomplete.** Every failure seen so far is classified below. The runs not done are listed in "Not done" at the end. No fstests guest is running and no images are left (`/var/tmp/btrfs-test/fstests-img/` is empty; 9.6G free on /var/tmp).

## Headline
- **Introduced by the branch (c): one real bug, not a deliberate refusal.** The branch forces copy-on-write for every NOCOW write whose extent is in a RAID5/6 block group (`can_nocow_file_extent()`, `fs/btrfs/inode.c:1913`; on the main branch since 7f036b7895). That includes the **data relocation inode**, whose writeback must go NOCOW into the extents relocation preallocated. A balance or convert that relocates RAID5/6 data can then fail with `-EINVAL`, and a balance that races with writers aborts the transaction and forces the filesystem read-only. It explains **btrfs/060, 062, 125, 195 and 348**. Confirmation: with the forced COW turned off (`btrfs.raid56_allow_nodatacow=1`, debug knob) on am2, 125 and 348 pass and 195's single-to-raid5 convert succeeds.
- **Fail on upstream too (a):** btrfs/197 and 198 (btrfs check of a degraded RAID6), btrfs/100 (lockdep, upstream lock order) and btrfs/146 (dm-error test, final fsck).
- **Harness/UML (b):** btrfs/207 `nospace` on am (host disk shared with other sessions), btrfs/062 `hung` on am2+knob (UML ubd lost a discard completion inside mkfs), lockdep switching itself off mid-run, and a summary-parser glitch. None of these belongs in `uml.expunge` yet (see each entry).

## Kernels

| tag | snapshot | source |
|---|---|---|
| up | `/var/tmp/btrfs-test/linux-up-snap` | `df2908090c` Linux 7.3-rc2 (the branch base), worktree `/home/user/BTRFS-up`, clean build `7.3.0-rc2-gdf2908090cda` |
| s0e | `/var/tmp/btrfs-test/linux-s0e-snap` | `5fe842c39a-dirty` (the stage-0 kernel whose fs/ code is on main) |
| am | `/var/tmp/btrfs-test/linux-am-snap` | strict-wip `61b9dce00a` |
| am2 | `/var/tmp/btrfs-test/linux-am2-snap` | strict-wip `830106b05c-dirty`, which appeared at 06:16 |

- **Config.** The upstream kernel used the same `.config` as the branch (`uml-s0w/.config`). `olddefconfig` changed nothing. The config has CONFIG_BTRFS_DEBUG=y and RUN_SANITY_TESTS=y, and no CONFIG_BTRFS_ASSERT.
- **Build.** It took 204 s at -j3 under nice 10.
- **Disk space.** `make clean` was later run in `/var/tmp/btrfs-test/uml-up`, because other work was filling the shared disk. `.config` is kept there and in `/var/tmp/btrfs-test/up-build.config`; a rebuild takes about 3.5 min.
- **am versus am2.** `fs/btrfs/inode.c`, `relocation.c`, `volumes.c` and `ctree.c` are identical between am and am2, and still identical at strict-wip c57f2f6fe0, where every line number below was checked.
- **am2 is a -dirty build.** Its uncommitted part is not known exactly. The next commit only touches `raid56-wib.c`, `raid56-wib.h` and the self-tests.

## Results

Result dirs are under `/var/tmp/btrfs-test/fstests-results/`, and the `.out` logs are in the scratchpad `fstests/` dir. The raid56 list has 30 tests. All raid56 runs used `BTRFS_PROFILE_CONFIGS="raid1:raid5 raid1c3:raid6 raid5:raid5 raid6:raid6"`.

**raid56 list: `raid56-up`, `raid56-s0e`, `raid56-am`, plus `raid56knob-am2`** (am2 with `btrfs.raid56_allow_nodatacow=1`, only 4 tests):

| test | up | s0e | am | am2+knob | class |
|---|---|---|---|---|---|
| btrfs/060 | pass | **fail** | pass | - | (c) forced-COW relocation bug, timing dependent |
| btrfs/062 | pass | **fail** | **fail** | hung (UML ubd) | (c) same bug |
| btrfs/125 | pass | **fail** | **fail** | pass | (c) same bug |
| btrfs/195 | pass | pass | **fail** | convert OK; fail only on a lockdep splat | (c) same bug |
| btrfs/348 | pass | **fail** | not reached | pass | (c) same bug |
| btrfs/197 | fail | fail | fail | - | (a) + our profile config, see below |
| btrfs/198 | fail | fail | fail | - | (a) same as 197 |
| btrfs/207 | pass | pass | nospace | - | (b) shared-disk space, not run on am |
| btrfs/261, 286, 297 | pass | pass | not reached | - | not run on am |
| the other 20 | pass | pass | pass | - | |

Wall times: up 4867 s in 1 boot; am 4665 s up to the space kill.

**volume list: `volume-up` and `volume-s0e`.** am and am2 were not run.

| test | up | s0e | class |
|---|---|---|---|
| btrfs/100 | fail | fail | (a) upstream lockdep report |
| btrfs/146 | fail | fail | (a) in this rig, probably rig related |
| btrfs/350 | not run | not run | FITRIM not supported on the scratch device |
| the other 25 | pass | pass | |

## (c) Introduced by the branch: forced COW breaks data relocation on RAID5/6

**Tests and failing steps:**
- **btrfs/125**, step "Mount normal and balance": `btrfs balance start` gives `ERROR: error during balancing '/run/fst/mnt/scratch': Invalid argument`.
- **btrfs/348**: after `btrfs device add`, `btrfs balance start -d` fails with EINVAL, so the test reports `failed: '.../btrfs balance start -d /run/fst/mnt/scratch'`.
- **btrfs/195** (am only): converting single to raid5 on 4 devices, balance fails with EINVAL, then `4:single:raid5: Failed convert` and `4:single:raid5: Scrub failed`.
- **btrfs/060 and 062**: a balance running next to fsstress (plus subvolume ops or defrag) aborts the transaction and the filesystem goes read-only. The scrub that follows fails with `errno=30 (Read-only file system)`, the test prints `Scrub find errors in "-m raid1 -d raid5" test`, and `_check_dmesg` flags the WARNING. 060 hit it on s0e but not on am (timing). 062 hit it on s0e and am, and on am in three profiles (raid1/raid5, raid1c3/raid6, raid6/raid6).

**Kernel messages** (am, btrfs/125):
```
BTRFS warning (device ubdb): inode 257 is NODATACOW but its extent at 2446327808 is in a RAID5/6 block group; copying instead of overwriting in place, because an in-place write there cannot be verified
   (the same message with the address +4096, +4096, ... : one 4 KiB write at a time)
WARNING: fs/btrfs/ctree.c:531 at btrfs_force_cow_block+0x40c/0x904, CPU#0: btrfs/11850
BTRFS error (device ubdb state A): Transaction 16 aborted (-EINVAL)
BTRFS: error (device ubdb state A) in btrfs_force_cow_block:531: errno=-22 unknown
BTRFS: error (device ubdb state EA) in merge_reloc_roots:2015: errno=-22 unknown
```
In 062 on am, the same abort at `ctree.c:531` is reached from three other tasks:
- fsstress calling fallocate (`btrfs_replace_file_extents:2439`),
- the ordered-extent worker (`btrfs_finish_one_ordered:3357`),
- balance.

The stack shows `btrfs_force_cow_block <- btrfs_cow_block <- btrfs_search_slot <- btrfs_lookup_file_extent <- btrfs_drop_extents`. `ctree.c` is not changed by the branch. Line 531 is the `btrfs_abort_transaction()` after `btrfs_reloc_cow_block()`. The only `-EINVAL` that `btrfs_reloc_cow_block()` can return comes from `get_new_location()`. "inode 257" is the data relocation inode.

**Root cause** (paths in strict-wip; the same code is on main):
1. `fs/btrfs/inode.c:1913-1920`, in `can_nocow_file_extent()`: `if (btrfs_logical_is_raid56(root->fs_info, io_start) && !btrfs_raid56_allow_nodatacow()) goto out;` forces COW for any inode, including the data relocation inode.
2. Relocation preallocates each destination extent in the data relocation inode (`relocation.c:2789`, `prealloc_file_extent_cluster()`). It relies on writeback going NOCOW into those preallocated extents. A partial writeback then only splits the file extent item, and the front item keeps offset 0 and the full `disk_num_bytes` (the comment in `get_new_location()` describes exactly this).
3. With COW forced, `run_delalloc_nocow()` takes `must_cow` (`inode.c:2171`) and then `fallback_to_cow()` (`inode.c:2229`). The new extent is only as large as the range being written: `cow_file_range()` sets `min_alloc_size = num_bytes` of that range at `inode.c:1403`. Relocation writes back 4 KiB at a time here (the consecutive 4 KiB addresses in the warnings above), so the new extents are smaller than the source extent.
4. When the subvolume leaves are later COWed, `btrfs_reloc_cow_block()` calls `replace_file_extents()` (`relocation.c:5918`), which calls `get_new_location()` (`relocation.c:1020`). There `if (num_bytes != btrfs_file_extent_disk_num_bytes(leaf, fi)) return -EINVAL;` (`relocation.c:922`) fails. The result is either an EINVAL returned to `btrfs balance`, or a transaction abort in whatever task COWs the leaf.

**Not a deliberate refusal.** The branch's own refusals are EIO or EPERM with their own messages. This is an internal size check failing, with a WARNING and a transaction abort, and it turns a healthy filesystem read-only in the middle of a balance.

The forced-COW rationale in the comment does not apply to the relocation inode:
- it writes into freshly preallocated extents that no subvolume references yet;
- its data carries checksums cloned from the source extent.

Upstream's `cow_file_range()` already warns that relocation extents must never be split. The branch turns that rare path (a block group made read-only by scrub) into the normal path for every RAID5/6 data relocation.

**Confirmation.** On am2 with `btrfs.raid56_allow_nodatacow=1` (the debug knob only disables this forced COW and the `chattr +C` EPERM refusal at `ioctl.c:271`):
- 125 and 348 pass;
- 195's convert succeeds (its only failure there is the lockdep splat described under (b));
- 062 could not be judged: the harness hung in mkfs.

**Suggested fix (not tested).** Exempt the data relocation root from the forced COW, e.g. add `!btrfs_is_data_reloc_root(root)` to the condition at `inode.c:1913`. The free-space-cache inode is probably in the same position, but that was not checked.

## (a) Fails on upstream too
- **btrfs/197 and btrfs/198**: identical on up, s0e and am. The final `_check_scratch_fs` runs btrfs-progs v7.1 `btrfs check` on the last workout's filesystem: RAID6 on 4 devices, with devid 2 made foreign (197) or wiped (198). It reports `warning, device 2 is missing` and `bad tree block 38944768, bytenr mismatch, want=38944768, have=0` (the same block every time). The kernel mounts and reads that filesystem degraded without complaint.
  - **Our configuration is what exposes it.** `_check_btrfs_raid_type` filters by `BTRFS_PROFILE_CONFIGS`, so our raid56 setting skips the raid10 workout. RAID6 therefore becomes the last workout, the one that gets fsck'd. With the default configs the checked filesystem is raid10.
  - Whether btrfs-progs' RAID6 rebuild in `read_raid56()` or the kernel's degraded write is at fault was not determined. It is pre-existing either way.
  - **Recommendation:** run 197 and 198 without `BTRFS_PROFILE_CONFIGS`, or drop them from `raid56.list`. They do not belong in `uml.expunge`.
- **btrfs/100**: lockdep "possible circular locking dependency".
  - The mmap side: `mmap_lock` then `sb_internal`, through `btrfs_file_mmap_prepare -> touch_atime -> btrfs_dirty_inode -> btrfs_join_transaction`.
  - The other side: `kernfs_rwsem` taken in `btrfs_sysfs_add_device` from `btrfs_init_new_device`, while `kernfs_fop_readdir` takes `mmap_lock` under `kernfs_rwsem`.
  - The same report appears on upstream, and only upstream code is involved.
  - It is order dependent: lockdep reports once per boot, and 100 passed in the earlier `smoke-s0e` run.
- **btrfs/146**: identical on up and s0e.
  - The step before the check runs `btrfs check --repair --force`, which warns `WARNING: filesystem mounted, continuing because of --force`.
  - Then `device-mapper: remove ioctl on error-test.146 failed: No such device or address`.
  - The final check of /dev/ubdb then fails: `ERROR: root [7 0] level 0 does not match 1 / could not setup csum tree`.
  - This is probably the rig (dm teardown without udev in the UML guest), but that is not shown. Confirm on a non-UML host before adding it to `uml.expunge`.

## (b) Harness and UML limits seen
- **btrfs/207 `nospace` on am.** Free space on /var/tmp dropped below the 5 GiB floor (`MIN_FREE_MB=5120`) because other builds and images on the shared disk were growing; this run's own images stay under about 1.6 GB. Boot 2 was killed at once because the space had not come back yet. That left 261, 286, 297 and 348 not reached on am. Not a kernel issue.
- **btrfs/062 hung on am2+knob, in the first `mkfs.btrfs`.**
  - Blocked stacks from the UML mconsole `sysrq w` are saved in `raid56knob-am2/hung-062-sysrq-w.txt`. The only blocked task is `mkfs.btrfs` in `blkdev_ioctl -> bio_submit_or_kill -> submit_bio_wait -> blk_mq_get_tag` (BLKDISCARD).
  - On the host, every UML thread was idle in poll: the ubd driver lost a completion.
  - The test was marked hung (`ctl/hung.1`) and the guest stopped instead of waiting for the 3600 s watchdog, so the runner's own resume boot ran 125, 195 and 348.
  - This happened once. It is not an expunge case: it is a rig flake that the watchdog would handle.
- **Lockdep switches itself off during the raid56 runs** ("BUG: MAX_LOCKDEP_CHAIN_HLOCKS too low!", on all kernels, around 061-063). Later tests get no lockdep checking, and lockdep failures depend on test order.
  - Example: on the knob run, 195 was early in a fresh boot and caught a splat of the same upstream pattern as btrfs/100, through `btrfs_sysfs_add_block_group_type` during balance.
  - Fix: a larger `CONFIG_LOCKDEP_CHAIN_BITS`, or read lockdep failures with care.
- **Summary parser glitch.** In fstests-uml.sh, the lockdep line `btrfs/2097 is trying to acquire lock:` (task name `btrfs`, pid 2097) was taken for a test name. That produced a bogus `btrfs/2097 fail` and turned 195 into `unknown` in `raid56knob-am2/summary.tsv`. The check output says `Failures: btrfs/195`. The test-name regex should be anchored to check's output format.
- **Boot-time self-tests.** Every boot of the am kernel spends about 25 s in the raid56-wib self-tests and prints `BTRFS error (device (efault)): raid56 write-intent log still full after 5000 ms, failing the write` five times. This is expected self-test output, not a failure (the review-fix round removed these waits; am2 boots in about 2 s).

## Commands (to repeat or resume)
Setup:
```
S=<session scratchpad>/fstests (runner not yet committed)
R=/var/tmp/btrfs-test/fstests-results
P56="raid1:raid5 raid1c3:raid6 raid5:raid5 raid6:raid6"
```
Upstream kernel:
```
git -C /home/user/BTRFS-linux worktree add --detach /home/user/BTRFS-up df2908090c
mkdir -p /var/tmp/btrfs-test/uml-up && cp /var/tmp/btrfs-test/uml-s0w/.config /var/tmp/btrfs-test/uml-up/
make -C /home/user/BTRFS-up ARCH=um O=/var/tmp/btrfs-test/uml-up olddefconfig
nice -n 10 make -C /home/user/BTRFS-up ARCH=um O=/var/tmp/btrfs-test/uml-up -j3 linux
cp /var/tmp/btrfs-test/uml-up/linux /var/tmp/btrfs-test/.up.tmp && mv /var/tmp/btrfs-test/.up.tmp /var/tmp/btrfs-test/linux-up-snap
```
Runs done (one at a time; each needs a new results dir):
```
MIN_FREE_MB=5120 BTRFS_PROFILE_CONFIGS="$P56" $S/fstests-uml.sh /var/tmp/btrfs-test/linux-am-snap  $R/raid56-am  @$S/raid56.list
MIN_FREE_MB=5120 BTRFS_PROFILE_CONFIGS="$P56" $S/fstests-uml.sh /var/tmp/btrfs-test/linux-up-snap  $R/raid56-up  @$S/raid56.list
MIN_FREE_MB=5120 $S/fstests-uml.sh /var/tmp/btrfs-test/linux-up-snap $R/volume-up @$S/volume.list
MIN_FREE_MB=5120 BTRFS_PROFILE_CONFIGS="$P56" $S/fstests-uml.sh /var/tmp/btrfs-test/linux-am2-snap $R/raid56knob-am2 \
    "btrfs/125 btrfs/195 btrfs/348 btrfs/062" btrfs.raid56_allow_nodatacow=1
```
The s0e runs: raid56 with the same `P56` and the default `MIN_FREE_MB=3072`; volume without `P56`.

Compare:
```
$S/fstests-uml.sh --compare $R/raid56-up $R/raid56-am
$S/compare3.sh [-a] $R/raid56-up $R/raid56-s0e $R/raid56-am $R/raid56knob-am2   # N-way
```
Queue helpers: `$S/queue2.sh` and `$S/drive-quick.sh`. `$S/mconsole.py <~/.uml/UMID/mconsole> <cmd>` is a minimal UML mconsole client (e.g. `sysrq 8`, then `sysrq w`, to dump blocked tasks of a stuck guest).

## Not done (paused)
1. **raid56 on am2 without the knob**: not started. For the forced-COW bug, am is the control: the relevant files are identical in am2.
2. **volume list on am and am2**. The am run was started, then stopped after about a minute when am2 appeared. Its partial dir `$R/volume-am-aborted` has no results. am2 was not started. So the volume list has **no strict-kernel coverage yet**. Only s0e matches upstream there.
3. **On am: 207 (nospace), 261, 286, 297 and 348 (not reached).** 348 is covered by s0e (fail) and am2+knob (pass).
4. **The knob run's 062**: hung in the harness, no result.
5. **197 and 198 without `BTRFS_PROFILE_CONFIGS`**, to confirm the profile-config explanation.
6. **The generic TEST_DIR quick subset**: 257 tests on a 3-device RAID5 TEST_DEV, on up and am2. Not run. Command:
   ```
   MIN_FREE_MB=5120 EXTRA_DEVS=2 TEST_MKFS_OPTIONS="-d raid5 -m raid1 /dev/ubdj /dev/ubdk" \
     $S/fstests-uml.sh <kernel> $R/quick-<tag> @$S/generic-testdir-quick.list
   ```
