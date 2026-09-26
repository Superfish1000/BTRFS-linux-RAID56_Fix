# RAID5/6 crash and fault-injection test rig

Boots a User Mode Linux kernel against image files or device-mapper devices,
drives a scenario inside it, and checks the filesystem from the outside. Used
to verify the RAID5/6 write-intent log and the integrity fixes: it can inject a
crash into the middle of a stripe write, fail or detach a device, and confirm
what a committed file reads back as afterwards.

## Setting up

Everything is driven by `BTRFS_TEST_DIR`, a scratch directory holding the
kernels, btrfs-progs and per-scenario disk images. Nothing is written inside
the kernel source tree.

    export BTRFS_TEST_DIR=/var/tmp/btrfs-test
    mkdir -p $BTRFS_TEST_DIR/umltest

Build two UML kernels from this source tree. The scenarios that inject a crash
need `CONFIG_BTRFS_DEBUG` for the `btrfs.raid56_crash_point` module parameter,
and the self-test boot needs `CONFIG_BTRFS_FS_RUN_SANITY_TESTS`:

    make ARCH=um O=$BTRFS_TEST_DIR/uml-fast defconfig
    # enable: BTRFS_FS=y BTRFS_DEBUG=y BTRFS_FS_RUN_SANITY_TESTS=y
    #         BLK_DEV_UBD=y BLK_DEV_DM=y DM_FLAKEY=y HOSTFS=y PROVE_LOCKING=y
    make ARCH=um O=$BTRFS_TEST_DIR/uml-fast -j$(nproc) linux

A second kernel with `KASAN=y` (as `$BTRFS_TEST_DIR/uml`) is worth having for
the device-failure scenarios; it is roughly three times slower.

btrfs-progs must be built and installed under `$BTRFS_TEST_DIR/progs-install`,
because the guest runs the host filesystem through hostfs and uses that copy.

## Running a scenario

`run3.sh` boots a scenario, then remounts once per omitted device so the
committed data is checked with each device missing in turn:

    ./run3.sh $BTRFS_TEST_DIR/uml-fast/linux r5-crash raid5:raid1 rw 1 4

Arguments are kernel, tag, `data:metadata` profiles, mount options, crash
point, and device count. Results land in `$BTRFS_TEST_DIR/umltest/results.<tag>`.

`PREPARE_MODE` selects what the first boot does. Beyond the default sub-stripe
write, `prepare_fsync` leaves data referenced only by the tree log, and
`inplace` overwrites a nodatacow file in place.

Scenarios can run side by side in one `BTRFS_TEST_DIR` as long as their tags
differ: each guest mounts a `/tmp` of its own over the host's, and the scripts
copy `init-final3.sh` and the Python helpers into `$BTRFS_TEST_DIR/umltest`
under a temporary name and rename them, so a guest never reads a half-written
copy. Those copies have fixed names, though: runs from two checkouts of this
tree need two `BTRFS_TEST_DIR`s, or one may boot the other's scenario code.

## Crash points

With `CONFIG_BTRFS_DEBUG`, `btrfs.raid56_crash_point` injects a failure into
the next recorded write:

| Value | Effect |
|-------|--------|
| 1 | drop the P/Q writes, then panic once the data writes land |
| 2 | drop the data writes, then panic once P/Q land |
| 3 | drop the P/Q writes, no panic |
| 4 | panic in the next parity scrub before its write |

## The other scripts

| Script | What it does |
|--------|--------------|
| `dmfail34.sh` | device-mapper faults: `detach` fails every I/O, `flakey` fails only writes |
| `degraded23.sh` | writes and crashes while a device is already missing, then verifies with it back |
| `stress23.sh` | concurrent fsync writers, SIGKILL at a random moment, then verifies every file whose fsync returned |
| `staleq.sh` | leaves a sector stale behind an accepted write, then reads it back |
| `misc3.sh` | single-boot scenarios: replace, convert, toggle, scrub, writers |
| `selftest.sh` | boots and reports the in-kernel btrfs self-tests |
| `split_status.sh` | two arms: does a degraded read return a reconstruction its checksum already rejected? |
| `unprovable.sh` | two arms: does scrub write a rebuild it cannot verify over good data? |
| `evidence_drop.sh` | eight arms: what the evidence channel does when it cannot keep a stripe -- ring full, stripe too wide, disarmed under a live capture -- plus the bound helper: one `evidence collect` keeps everything, `DISARM_IF_EMPTY` refuses a non-empty ring, a killed reader disarms the channel |
| `rmw_repair.sh` | seven arms: does the next write into a damaged stripe repair it, is a write into an undecidable stripe refused, does the repair queued on the fault put the stripe back with nothing else writing? Each against a control that restores the old behaviour |
| `rmw_cache.sh` | three arms: does a write served from the stripe cache into a recorded stripe lose the stale column when its own parity write fails? And when that write's flush fails on the parity device, is the parity nothing wrote to named stale (the read of the column refused)? |
| `alert.sh` | two arms: does every channel (kernel log, `raid56_health` + poll, uevent, fanotify, EIO) report a degraded -> failing -> recovered episode, and stay silent without a fault? Needs CONFIG_FANOTIFY. |
| `rmw_torn.sh` | two arms: does a crash in the write that repairs a stale column lose that column? |
| `repair_pin.sh` | two arms: a repair held in flight while its block group is balanced away and the space refilled -- does it write into the new data? |
| `repair_freeze.sh` | two arms: does the queued repair write to a frozen filesystem? |
| `stale_read.sh` | two arms: does an ordinary read of an unchecksummed block the record names as stale return the old content? |
| `mixed_unchecked.sh` | six arms: in a `--mixed` RAID6 block group, is nodatasum data that only an unverifiable rebuild can produce (Q alone at mirror 3, or a column recorded stale) refused with an alert rather than returned -- and by relocation too (`device remove missing`), rather than copied into the new chunk? Each against the old behaviour |
| `replace_rmw_resident.sh` | two arms: RAID5 on disks holding random bytes; a write into a full stripe the device replace has already copied, then the column's device lost -- does the write give the new device the old one's sectors its parity was computed from, or does the data read back wrong with no error? Uses `raid56_chunks.py` |
| `scrub_unrepaired_stop.sh` | two arms: does a RAID5 scrub go on past a full stripe it cannot repair whose last data column holds no extent, and regenerate a wrong parity further on the same device? |
| `early_record.sh` | six arms: RAID6 with one device missing and a column of another stale -- does the mount read its tree roots, and the file, with the record in hand? ro and rw, each against the old behaviour, and against taking every record for a write that may have been torn |
| `torn_readd.sh` | two arms: a write a failed flush may have torn, whose record could not name the device, then a crash and a degraded mount -- is the rebuild of the missing column refused (ro and rw), or read back as data because the record reads like a plain failed write? With `upgrade` as the third argument, the same with the log a kernel from before the mark leaves (plain records, no marker in its blocks): refused, against trusting it. With `upgrade-verdict`, that arm against counting the recovery's verdicts as stale parities: the log goes wide, verdicts are spent (`sticky_evicted`) and rows read back wrong |
| `readd_flush.sh` | three arms: a device drops acknowledged writes, then fails the commit's barrier, then a crash -- are the kept stripes named, so that every acknowledged block reads back, or kept unnamed and read back old? And with `raid56_wf_finished_stay_inflight=1`, listed in flight as well, so that the mount takes them for possibly torn and the blocks on the device read EIO? With `disable` as the second argument, the same with that commit the one writing the last block of a log being disabled, against the disable that forgot what its failed barrier dropped |
| `degraded_csum.sh` | two arms: RAID6, a crash while degraded tears a stripe whose missing column holds a checksummed sector that no rebuild matches -- is that sector left unrepaired, the column's other (unchecksummed) blocks read back, or is the whole stripe recorded undecidable and those blocks refused? Uses `raid56_csum_layout.py` |
| `replace_torn_free.sh` | two arms: a crash tore a RAID5 full stripe whose free data column is on the device that then goes missing; mounted degraded, the recovery keeps the stripe marked possibly torn -- does the replace of that device rebuild the free sectors onto the new device, or refuse the rebuild as a guess from a torn parity and count them as lost (`replace_uncopyable`, zeros on the new device, a stale mark)? Against the old behaviour |
| `replace_marks_kept.sh` | ten arms: a replace of a missing RAID5 device that cannot rebuild some sectors -- in a log full of the degraded writes' records (`keep`), are its records of the zeros it puts on the new device kept, or does the next one spend the first and the zeros read back as data? And with more such records than the log holds (`lost`), does the replace fail with an alert, or finish with zeros read back as data? Where a degraded write had named the column stale already (`stale`), is the record the replace's all the same, or spent first as not its own? And after a crash in the middle of the replace (`resume`, two boots, `replace_marks_rows.py` picks the rows), does the resumed replace copy again from the start, recording the zeros as its own again, or resume past them and let their reloaded records be spent? And when the replace resumed at mount after a crash fails on purpose (`resume-lost`, `lost` in the second boot), does it say so, or raise a kernel warning with a backtrace as for a bug? Each against the old behaviour |
| `verdict_keep.sh` | two arms: the recovery's verdicts on torn stripes it could not decide (the log of `torn_readd.sh`'s upgrade arm, a device missing), then a device replace that finishes (`replace`) or a wide log block and a remount (`remount`), then writes into new regions of the full log -- do the verdicts stay, spent last, or are they spent and the rows read back as a rebuild from a torn parity? With `flush`, the remount followed by a present device failing a log flush after writes that finished in the verdicts' regions: does the readd leave the verdicts theirs, or take them over (`raid56_wf_readd_disowns_all`)? Each against the old behaviour |
| `recover_interrupt.sh` | three arms: a crash tore a stripe whose data column is on the missing device, behind stripes of another file the log also records; `mount -o ro,degraded`, then `remount,rw` with the recovery lingering on each stripe -- is the column refused while the recovery runs, after the remount is SIGKILLed and fails, and while a second remount's recovery lingers on the torn stripe itself (taken over, not decided yet), or read back as the rebuild from the torn parity? Against the old behaviour: the refusals dropped up front (`control`), or only the stripe being decided left unrefused (`selfctl`, `raid56_wf_recovering_stripe_readable=1`) |
| `replace_abort.sh` | two arms: a RAID5 replace aborted because it cannot record a sector it can neither copy nor rebuild (mounted `-o noraid56_write_intent`) -- does `raid56_health` read failing, `replace_aborted` unacknowledged, until `echo ack`, or ok at once? Against the old behaviour |
| `torn_present.sh` | ten scenarios, each against the knob that restores the old code: a full stripe a write may have torn, with no parity left over to check a rebuild -- a column the record names with every device present (`named`), the classification's verify pass with a device missing (`classify`, scored on the platter), a sector that does not read after the recovery kept the stripe undecided (`unreadable`), a column named afterwards on a device that fails a barrier (`readd`) or the write itself (`fault`), whose repair would rebuild it from P alone: is the rebuild written back or read as data, and is the stripe the recovery cannot decide alerted (torn_undecidable, read_unrecovered, stripe_undecidable)? And with the unreadable stripe kept, a log a failed barrier fills with records that say no more than that a write may have been torn, then a write into a new region (`evict`): does it spend one of those, or the recovery's record, whose refusal the unreadable sector needs (`raid56_wf_kept_torn_in_order`)? And a RAID6 stripe the recovery regenerated P of with Q's device missing, then a sector that stops reading (`misspar`): is the exact rebuild from P read back, or refused because the stripe was kept possibly torn? And the same with the torn column's device left out instead (`decided`), both parities regenerated once they agree about it: are B and that column rebuilt from the two, or refused (`raid56_wf_absent_decided_keeps_torn`)? A stripe kept possibly torn that a failed write then names a column of (`runtime`): does the refusal say that no scrub can decide it and how to clear it, and does the scrub that leaves it untouched raise `torn_undecidable`, or is a scrub promised that changes nothing? A stripe recorded undecidable with every device present (`clear`): once the file is deleted and a scrub run, does a new file written over the same stripe land, or are its writes refused for good? |
| `recover_scrub.sh` | seven arms: does the mount's recovery repair what the record proves, keep what it cannot decide? Checks the platters directly (`nocow_platter.py`). The `quiet` arm runs no background repair before the unmount, so nothing but the failed barriers' blocks decide what the log lists; the `inflight` arm is `quiet` with `raid56_wf_finished_stay_inflight=1`, which leaves the finished writes listed in flight across the clean unmount, and the recovery keeps their stripes undecided as possibly torn. The `remount` arm keeps the device failing until every repair has retried and given up -- writes recorded after the last commit, finished -- then heals it and remounts read-only before the unmount; `remountctl` is the same with `raid56_wf_remount_ro_keeps_inflight=1`, which leaves the retries listed in flight |
| `flush_wedge.sh` | three plans: a device drops writes under one overwrite per region, then fails a commit's barrier -- does the log full of the records that leaves wedge every write into a new region? `named` (repairs retire them, against `raid56_wf_readd_no_repair`), `torn` (spent last with the alert, against `raid56_wf_torn_unevictable`), and `busy`: eight writes at once into a log full of possibly torn records with one slot left -- do they wait for the one in flight, or spend them (`raid56_wf_torn_spent_eagerly`)? |
| `readd_admit.sh` | three arms: an owed readd while writes into new regions keep coming (`new`, against `raid56_wf_readd_admits_new`), writers keeping the regions it waits for busy (`hot`), and how often its waits and their ends are said in the kernel log while a device keeps failing flushes (`say`, against `raid56_wf_readd_says_each`) |
| `nocow_persist.sh` | three arms: does the stale record survive a mount, and is an ambiguous stripe declined? |
| `age.sh` | ages a RAID5/6 filesystem and measures its stranded free space |

`init-final3.sh` runs as init inside the guest and holds every scenario; the
host scripts only choose one and pass parameters on the kernel command line.

## Known gaps

Every behaviour change has a `btrfs.raid56_*` knob that restores the old
behaviour.  These have no UML arm whose control shows the old defect; only the
kernel self tests (`fs/btrfs/tests/raid56-wib-tests.c`, run by `selftest.sh`)
cover them, where any does:

| Knob or change | Why there is no UML arm | Self test |
|----------------|-------------------------|-----------|
| `raid56_wf_snapshot_misses_marks` | needs a repair's log write to land between `write_all_supers()`'s snapshot (`btrfs_wib_commit_prepare()`) and its `btrfs_wib_commit()`, a window no scenario can hold open | `test_persist_folds_snapshot()` |
| the readd's WAIT and LOSE plans against `raid56_wf_no_readd_name` | the log's last block must list nearly 165 regions while marks the block does not list are in flight at the failed flush, with the failing device holding members of them; `readd_admit.sh` reaches WAIT, and the take-back once the device is healed (control `raid56_wf_readd_admits_new`), with a device that holds no member of the file's stripes, so no name is at stake there and no arm sets `raid56_wf_no_readd_name` | `test_readd_keeps_records()` (WAIT; under the knob it asserts the loss), `test_readd_names()` (WAIT then NAMED, LOSE) |
| the bound on an owed readd's WAIT (`BTRFS_WIB_READD_WAIT`, 30 s: LOSE, "not waiting for them any longer", `record_dropped`) | the readd has to stay owed for 30 s -- the device failing flushes, or the writes holding the readd's room not draining, for that long; `readd_admit.sh` fails the flushes for 5 s (`RA_FAIL_SECS`) and the readd takes the records back within about a second of the heal, so no arm (`new`, `hot`, `say`) ever gives up, and its control (`raid56_wf_readd_admits_new`) lifts the bound as well | `test_readd_wait_bounded()` |
| `raid56_wf_readd_admits_busy` | needs writes into a region the owed readd waits for to overlap for `BTRFS_WIB_READD_WAIT` (30 s): UML records no more than about four writes at once (one CPU; read-modify-write reads queue on the ubd threads), so the region drains within a second under either rule. `readd_admit.sh hot` runs the busy writers against the fixed rule as a liveness check and reports the control unscored | `test_readd_busy_region()` |
| the unmount half of `raid56_wf_finished_stay_inflight` (`btrfs_wib_unmount()`) | needs a write recorded after the last transaction commit into a stripe whose record names a column -- a repair retried while the device still fails, after the last `sync` -- then a heal and an unmount that commits nothing; the `inflight` arm's control shows the failed-barrier half. `recover_scrub.sh`'s `remount` pair lets every repair retry and give up first, and reaches `btrfs_wib_unmount()` through the remount read-only, against its own knob | `test_unmount_drops_finished()`, `test_remount_ro_drops_finished()` |
| the replace half of `raid56_wf_readd_disowns_all` (a running replace's record of the zeros on its target taken over by a failed flush's readd) | `verdict_keep.sh flush` shows the recovery's verdicts taken over; the same with a replace needs a replace running, degraded, with its records in regions a write finished in, when a present device fails a log flush | `test_readd_keeps_verdicts()` |
| the resume from the start of a replace whose record of zeros a full log dropped (`btrfs_run_dev_replace()` writes the item with its cursor at 0 once `btrfs_wib_replace_marks_lost()` is set; no knob) | needs an unmount, a remount or a crash between that eviction and the copy loop failing the replace; `replace_marks_kept.sh resume-lost` crashes before the replace resumes, and its resumed replace fails without another boot | none |
| a user scrub under `raid56_scrub_torn_trusts_parity` (`SCRUB_WIB_TORN` of a stripe the live table marks possibly torn, not the recovery's) | `torn_present.sh runtime` reaches it -- the scrub leaves the stripe untouched and raises `torn_undecidable` -- but its control is `raid56_wf_torn_remedy_legacy`; only the recovery's scrub (`named`, `classify`) runs against this knob | none |
| `btrfs_wib_mark()`'s last try once the log-full wait has run out: a record that says only that a write may have been torn is spent rather than fail the write (no knob of its own; `raid56_wf_torn_spent_eagerly` spends at once) | needs a log full of such records while a repair stays queued for `raid56_log_full_repair_wait_ms`, or a write into a new region stays in flight for `BTRFS_WIB_FULL_TIMEOUT`; `flush_wedge.sh torn` spends one at once, nothing being in flight or queued | none |
| the written records a read repair notes (`btrfs_wib_note_written_data()` from `btrfs_repair_io_failure()` and `btrfs_submit_repair_write()` in `bio.c`) | no knob of their own (`raid56_wf_name_unwritten=1` makes the readd ignore every written record); a UML arm needs a read repair written back without FUA to a device that then fails a flush before a crash | the `k_repair` case of `test_readd_names()`, which calls `btrfs_wib_note_written_data()` directly: removing the `bio.c` calls fails no test |

After a commit whose barrier a device failed, the block written was a union
with the last one, which still listed in flight the writes that had finished
before the barrier was issued -- and nothing dropped them but a later barrier
every device confirmed, which a clean unmount does not issue when no
transaction is running.  A mount then took their stripes for possibly torn
and, where the record names a column only the parity can rebuild, kept them
undecided and refused the column: `recover_scrub.sh`'s fixed arm kept 24
records it could have repaired (unless a repair landed after the heal and
flushed every device first), `readd_flush.sh`'s fixed arms read 6 of 24
acknowledged blocks as EIO, and its `disable` pair 12 blocks nobody overwrote.
The names say what the barrier may have lost, and the parity the other devices
flushed rebuilds it: the block now lists those writes as the error records
they are (`wib_readd_base()`), and an unmount writes the log once more if it
still lists a finished write in flight (`btrfs_wib_unmount()`).  The arms fail
on any EIO or kept record; `raid56_wf_finished_stay_inflight=1` restores the
old behaviour for their controls.

## Measuring stranded free space

`age.sh` answers a design question rather than testing a fix: how much free
space in a real RAID5/6 filesystem sits inside vertical stripes that still
hold live data.  That free space is what an allocator forbidden from writing
into an occupied stripe -- the rule copy-on-write parity needs -- would have
to give up, and it is also the free space today's allocator can only use by
doing a read-modify-write into a stripe holding committed data.

    ./age.sh $BTRFS_TEST_DIR/uml-fast/linux age-r5 raid5:raid1 4 2G 0.80

Arguments are kernel, tag, `data:metadata` profiles, device count, device
size and the fullness to age at.  It boots once, fills the filesystem to that
fullness and then churns there (`age-workload.py`: delete whole files, refill,
overwrite survivors in place), dumps the chunk and extent trees from inside
the guest, and runs `../raid56_row_occupancy.py` over them.  An almost-empty
filesystem strands nothing, so the fill fraction is the parameter that
matters.
