.. SPDX-License-Identifier: GPL-2.0

=============================
BTRFS RAID56 write-intent log
=============================

The RAID5/6 write hole
======================

A write that does not cover a whole RAID5/6 full stripe is a
read-modify-write (RMW): btrfs reads the full stripe, computes the new P/Q
sectors and writes the new data sectors and the new P/Q sectors as
independent requests to independent devices.  If the system crashes after
some of these requests reached their device and others did not, the affected
vertical stripes are inconsistent: the parity does not match the data.

The new data itself is not at risk.  btrfs is copy-on-write, so the sectors
being written were free space and the transaction that would reference them
never committed.  The problem is the *other* sectors of the same vertical
stripes, which belong to committed extents: their content on disk is intact
but their redundancy is gone.  If the device holding one of them fails or
returns a bad sector, reconstruction from the stale parity produces garbage.
With data checksums this is detected as an unrecoverable checksum error; for
``nodatasum`` data it is silent corruption.

Writes that go in place (``nodatacow`` files, preallocated extents) are
affected even when they cover a full stripe: the sectors being overwritten
are referenced.

Before this feature btrfs had nothing to close this window.

The fix
=======

Every writable device carries a small log (two 4KiB blocks at physical
offset 512KiB, inside the first megabyte that btrfs reserves on every device)
listing the full stripes that have such a write in flight, with 64KiB
granularity of the logical address space.  Each listed stripe is either a
plain record (a write in flight on a working array) or an error record (see
below).

Runtime protocol
----------------

1. Before a sub-stripe RMW (or an in-place write) submits any write, the full
   stripe is added to the in-memory set and a block listing the union of the
   previous block and the set is written to every writable device with FUA.
   The write proceeds only after that log write completed on enough devices
   to survive the number of device failures the RAID profile tolerates;
   otherwise it fails with EIO before touching the stripe.  Concurrent RMWs
   share one log write.  Because the block only adds to the previous one, no
   device cache has to be flushed for it.
2. When all writes of the RMW have completed, the stripe leaves the in-memory
   set.  The on-disk log drops it at the next transaction or log commit: the
   set is snapshotted before the commit's device barriers, every device is
   asked to flush its cache, and only if every device confirmed are the
   stripes that had finished before the snapshot dropped, so their data and
   parity writes are on stable media before the log stops mentioning them
   (a stripe finishing during the flush stays listed).  If a device did not
   confirm, nothing is dropped and the finished stripes become error records
   (that device may hold stale sectors).  A log write that finds the union
   too large for a block flushes the devices itself and drops the finished
   stripes in the same way.
3. Each device alternates between its two blocks and only moves on after a
   successful write, so the block being overwritten on a device is never its
   newest valid one.  A torn log write can only invalidate the newest block;
   the previous one is still valid and, by the two rules above, lists every
   stripe whose writes can be unflushed on that device.
4. A RMW whose writes hit a device error (or a missing device) leaves the
   stripe inconsistent on that device without any crash.  Such a stripe is
   kept in the log as an error record so that it is scrubbed at the next
   mount, once the device is back, replaced or dropped.  A full stripe write
   that a device did not take is recorded the same way.  Error records are
   evicted, with a warning, only when the log is full; ``btrfs scrub`` covers
   them then.
5. A log write is allowed to fail on some devices: a device that did not
   receive the new block keeps the previous one, which lists a superset of
   the stripes it may hold unflushed.  Only the write that records a new
   stripe has to reach enough devices, see 1.

Full stripe copy-on-write writes need no record: every sector of the stripe
is unreferenced until the transaction referencing it commits, which happens
only after all those writes completed and were flushed by the commit's device
barriers.

Recovery
--------

At mount, before anything else is written (this includes a read-only mount
that has a tree log to replay, since the replay writes), the log blocks of
all present devices are read.  Per device the newest valid block counts
(valid = correct magic, this filesystem's uuid, correct checksum): a block
with sequence number N was written to that device with a flush whenever it
dropped a stripe listed by N-1, so what N no longer lists is on stable media
on that device.  Across devices the union is taken: a device that missed a
commit (torn write, I/O error, absent at the time) still lists the stripes
that commit dropped.

Every listed full stripe is then scrubbed (``btrfs_scrub_raid56_full_stripe()``
in ``fs/btrfs/scrub.c``): every sector holding an extent is verified (data
checksums, tree block headers for metadata), bad sectors are rebuilt from the
existing parity and written back to their device, and P/Q are recomputed from
the verified data for the vertical stripes that hold extents.  If a sector
cannot be repaired the parity of that full stripe is left alone: recomputing
it from unverified sectors would turn a still recoverable stripe into
consistent garbage.

For a plain record on a fully present array, every device holds what was
last written to it, so after that verification the parity of *every*
vertical stripe is recomputed from the data on disk.  This also covers
extents that are only referenced from the tree log at that point (fsync'ed
data and the log tree blocks themselves): they are not in the extent tree
until the log has been replayed, and a crash in the middle of a RMW to the
same vertical stripe leaves them without valid parity otherwise.

An error record is recovered exactly as a user scrub would repair it
(``BTRFS_RAID56_RECOVER_SCRUB``): the record is loaded into the live table
first, since that is what the scrub decides from, and a column it names as
stale is rebuilt from the parity even where the content has no checksum; a
stripe the record cannot decide (the columns it names outnumber the usable
parities, or part of the record was dropped when the log filled up) is
declined and stays recorded.  The record is retired only when the repair and
the new parity reached the disk.  Earlier versions only verified error
records at mount and kept every one of them, which left the stripe without
redundancy until someone ran a scrub, even though the same record was enough
to repair it (``tools/testing/btrfs/uml/recover_scrub.sh``; the old behaviour
is ``btrfs.raid56_recover_legacy=1`` on debug kernels, its negative control).

While a tree log is still to be replayed (so that extents may be hidden
from the verification) error records are only verified: the sectors holding
extents are verified and repaired but no parity is written at all, since the
scrub recomputes the parity of a vertical stripe from every sector in it,
including ones it cannot verify, which could destroy the parity an extent
hidden in the log still needs.  After the tree log has been replayed the
remaining error records are recovered as above, with every extent visible.
A stripe with a device missing, or with a sector that could not be repaired,
stays recorded and is scrubbed again at the first mount with the device back
or replaced.

Scrubbing a consistent stripe is a no-op, so stale entries (from an old
device that re-joined the filesystem, or from the older of the two blocks)
are harmless.

Feature flag and enabling
-------------------------

The log is tied to the ``compat_ro`` feature flag
``BTRFS_FEATURE_COMPAT_RO_RAID56_WRITE_INTENT`` (bit 4): a kernel that does not
maintain the log must not write to the filesystem, otherwise the log would be
stale after a crash.  Kernels without support therefore mount such a
filesystem read-only, and ``btrfs check --repair`` of a btrfs-progs without
support refuses to open it read-write.

The flag is set automatically at the first read-write mount of a filesystem
that has RAID5/6 block groups (``BTRFS_FEATURE_INCOMPAT_RAID56``), and by the
transaction that creates the first RAID5/6 chunk of a filesystem that had
none (for example ``btrfs balance -dconvert=raid5``), whose commit starts the
log before it writes the superblock.  A message is printed when this
happens.  To opt out, mount with ``-o noraid56_write_intent``; this only
prevents the automatic enabling, a filesystem that already has the flag
keeps its log maintained.

The flag can also be set or cleared on a mounted filesystem through sysfs
(or ``BTRFS_IOC_SET_FEATURES``)::

    echo 1 > /sys/fs/btrfs/<uuid>/features/raid56_write_intent
    echo 0 > /sys/fs/btrfs/<uuid>/features/raid56_write_intent

Setting it persists everything that is in flight before the flag is written.
Clearing it keeps the log maintained until a superblock without the flag is
durable (the commit after the one that writes it), so that a crash in
between is still recovered by a kernel that sees the flag; later RAID5/6
chunks do not turn it back on, the next read-write mount does unless the
mount option is given.

Statistics are exported in ``/sys/fs/btrfs/<uuid>/raid56_write_intent``.

Cost
----

A sub-stripe RMW pays one 4KiB FUA write per device (batched across
concurrent RMWs) before its own writes.  A transaction or log commit pays one
4KiB write per device after the barriers it issues anyway, only if the
in-flight set changed since the last log write.  Recovery reads
and rewrites the parity of at most as many full stripes as were in flight at
the crash.  The log holds 165 regions of 4MiB; a RMW that finds it full waits
for in-flight RMWs to finish.

Limits
======

* A device already missing when the crash happens, or a device that failed
  a write to the stripe shortly before it: a vertical stripe whose missing
  or stale sector is referenced data and whose parity was being updated by
  the crashed write has two faults, beyond what RAID5 (one) tolerates, and
  cannot be reconstructed.  The rebuild is verified against the data
  checksum (or the tree block header), so this is a detected loss, not
  silent corruption, except for ``nodatasum`` data.  If the lost block is a
  tree log block the mount fails with an I/O error at the log replay, as it
  does today, and ``btrfs rescue zero-log`` discards the log.  Closing this
  case needs a journal of the data itself, which the log does not provide.
* ``nobarrier``: the log writes are issued without FUA/PREFLUSH like every
  other write, and the guarantee is only as good as the device's volatile
  cache ordering.
* Zoned devices are not supported (RAID5/6 is not supported on them either).
* A device that acknowledges a flush and loses the data anyway is outside
  what any of this can protect against.

Filesystems that were already damaged
=====================================

The log protects a filesystem from the moment it is enabled.  An array that
was written by a kernel without it, and crashed, carries inconsistent stripes
that no record describes.  Nothing can say which side of such a stripe is
wrong -- that is what the record was for -- but three things are still
possible, and this is what they are for.

Finding the damage without a record
-----------------------------------

A scrub compares the parity it reads against the parity it computes from the
data on the same disks.  That is arithmetic on what is already there: it needs
no record, so it works on an array damaged by any older kernel.  Two counters
in ``/sys/fs/btrfs/<uuid>/raid56_write_profile`` report it::

    parity_mismatch_vertical_stripes   vertical stripes whose parity did not
                                       describe the data
    parity_mismatch_full_stripes       full stripes containing at least one

A non-zero count on an array that has never lost a device is the write hole's
footprint.  The data is still readable -- every device holds what was last
written to it -- but the redundancy of those stripes is gone: if a device
fails now, reconstruction from that parity produces a value nobody wrote.
With data checksums that is detected; for ``nodatasum`` data it is not.

Naming the members, when there is a record
------------------------------------------

Where the log does have records, ``BTRFS_IOC_RAID56_STALE_STRIPES`` hands them
to userspace: the regions recorded, and for each whether the log can name the
data column whose write failed (``stale``), knows only that something went
wrong (``sticky``), or knows the parity does not describe the data
(``stale_par``).  ``tools/testing/btrfs/wibdump.c`` is a reference reader.  A
named member is repairable; an unnamed one is the ambiguous case below.

Each region also carries the newest filesystem generation at which any block in
it gained a fault record, and a helper has to honour it.  Everything else a
record says can be re-derived by reading the disks; this cannot.  A logical
address is reused once its extent is freed and reallocated, so an extent found
there that is *newer* than that generation was written after the record and the
record does not describe it.  Repairing it on the record's say-so would damage
data that was never at risk.  The evidence header carries the same generation
for the same reason.

Copying out an ambiguous stripe: the evidence channel
-----------------------------------------------------

A scrub that reaches a stripe it cannot decide leaves it alone -- recomputing
the parity would destroy the only surviving copy of what was acknowledged, and
rebuilding the data would invent a value nothing committed.  At that instant
the scrub is holding the only mutually coherent copy of the stripe's data
columns that will ever exist: freshly read, with the block group read-only for
the whole chunk scrub so nothing can be writing, and about to be dropped.

``BTRFS_IOC_RAID56_EVIDENCE`` copies it out.  ``ARM`` before the scrub, ``READ``
repeatedly while it runs, ``DISARM`` afterwards; each ``READ`` returns one full
stripe's data columns and a header naming every column's devid and physical
offset.  Nothing is allocated until a helper arms it, so an unwatched
filesystem pays nothing.

Read *during* the scrub, not after.  The kernel holds four slots; waiting until
the scrub finishes means waiting for them to have overflowed.  A capture that
finds the ring full waits up to ``btrfs.raid56_evidence_wait_ms`` (5 s by
default) for the helper rather than discarding the stripe, and the ring never
overwrites the entry a helper has not read yet -- the oldest entry is the one
it is about to take.  Four counters come back with every ``READ``::

    captured        stripes copied out since ARM
    waited          of those, how many had to wait for a free slot -- non-zero
                    means the helper is only just keeping up
    dropped_full    stripes lost because it did not keep up at all
    dropped_wide    stripes too wide to copy: more columns than the header
                    can name, or more data than a slot holds (sixteen
                    columns' worth, which an 18-device RAID5 exceeds)

``dropped_full`` and ``dropped_wide`` are the count of evidence that no longer
exists anywhere.  They are reported rather than hidden precisely because there
is nothing else to be done about them afterwards.

The parity is deliberately **not** copied.  It is named instead -- devid and
physical offset per column -- and the block group stays read-only until that
chunk's scrub ends, so a helper reads the parity straight off the device
without racing anything.  Issuing more reads to an array that is failing by
construction, from a path with no ``REQ_FAILFAST`` and no timeout, is the
alternative that was rejected.

The header carries ``BTRFS_RAID56_EVIDENCE_F_COHERENT``.  It is set when the
block group was held read-only for the capture, which is the case on the
user-scrub path.  The write-intent log's mount-time recovery reaches the same
verdict with no such hold, so its columns may have been read either side of a
write; that capture is still made -- it is the earliest moment those bytes
exist together, and a crash nobody was awake for is exactly the case this is
for -- but the flag is clear and a helper must treat it as a best-effort copy.

``tools/testing/btrfs/evidence.c`` is a reference consumer: it drains to a
directory (the "recovery drive") while the scrub runs, one ``.data`` file of
the data columns and one ``.meta`` file per stripe.

What this does not do
---------------------

None of it repairs anything by itself, and none of it writes to the array.  It
copies out and describes; reconstructing a file from a captured stripe -- by
trying a rebuild from each candidate parity, or from each candidate stale
column, and checking the result -- is offline work for a userspace tool on the
copy, not something the kernel guesses at.  That is the whole point: a stripe
reaches this path precisely because there is nothing left that can decide it.

Failure points
==============

Every point at which the protocol can be interrupted, and what happens:

========================================  ========================================================
Failure                                   Outcome
========================================  ========================================================
Crash before the log write                No stripe write was issued; nothing to recover.
Crash during the log write (torn block)   Same; the older block stays valid, RMW never started.
Log write I/O error, enough copies left   RMW proceeds; the failing device keeps its older block,
                                          which lists a superset, and counts an error.
Log write I/O error, too few copies       RMW fails with EIO before writing, no inconsistency.
Log full                                  RMW waits for in-flight ones; error records evicted.
Crash after some data/parity writes       Stripe listed; scrubbed and parity regenerated at mount.
Torn data or parity sector                Same; the regeneration reads what is on disk.
Write error on a data or parity device    Stripe stays listed (error record) until scrubbed.
Device pulled during writes               As above; scrubbed at the next mount, by a device
                                          replace, or by any scrub -- see below.
Crash after all writes, before the clear  Stripe listed; the scrub is a no-op.
Crash during the clear (torn block)       Older block valid, same as above.
Clear write I/O error                     Device keeps the older block, which lists a superset.
Flush not confirmed by a device at commit Nothing is dropped; the finished stripes are kept as
                                          error records until a later commit's flush succeeds
                                          everywhere or the next mount scrubs them.
Crash during the recovery itself          Log still lists the stripe; redone at the next mount.
Unreadable or stale sector at recovery    Rebuilt from parity and verified by checksum, written
                                          back; if unrepairable the parity is left alone and
                                          the stripe stays recorded.
Parity write fails during recovery        Stripe stays recorded (even within RAID tolerance).
Repair write-back fails during recovery   Same.
Unreadable sector without extent          Parity of that vertical stripe left alone (after the
                                          log replay, if any); the record is dropped.
Device missing at recovery                Missing sectors rebuilt and verified; parity written
                                          to the present devices; the stripe stays recorded
                                          until something scrubs it again -- see below.
Extent only in the tree log at recovery   Plain record: parity of every vertical stripe
                                          regenerated, covers it.  Error record: scrubbed again
                                          after the log replay.
Crash during a tree log replay            The replay's own writes are logged (also on a
                                          read-only mount); recovered at the next mount.
Log block unreadable on a device          Device skipped, the others provide the log.
Recovery of a stripe fails otherwise      (corrupt extent tree leaf, ...) The stripe stays
                                          recorded, the mount proceeds; only a memory
                                          allocation failure fails the mount.
Write recorded during the commit's flush  Listed in the snapshot the commit drops against, so
                                          it stays listed until the next commit's flush.
Read-only mount without a dirty log       Nothing written; recovery runs at remount read-write.
Read-only media (all devices read-only)   Nothing written; a dirty tree log fails the mount as
                                          before.
Device add/remove/replace during writes   Log writer never takes the device list mutex; it
                                          holds a reference on each block device it writes.
RAID5/6 chunk created at runtime          Log enabled by the transaction that creates the chunk,
                                          before its superblock is written.
Conversion away from RAID5/6              Log stays enabled and harmless.
Feature flag cleared through sysfs        Log maintained until the superblock without the flag
                                          is durable.
In-place (nodatacow/prealloc) write       Recorded even when it covers the full stripe.
Older kernel                              Mounts read-only (compat_ro flag).
Degraded before the crash                 Documented limit: detected, not silent (see above).
========================================  ========================================================

Reading with the record
=======================

A column the record names stale holds the old content on its device while the
parity holds what was acknowledged.  For a block with a checksum that is
harmless -- the checksum fails and the read is repaired -- but a block with no
checksum has nothing to fail, so every read path has to consult the record
itself:

* a read that goes through the RAID5/6 rebuild (a missing device, a failed
  checksum, a read-modify-write) marks the named columns failed before
  rebuilding, but only when the rebuild still fits the parity that is left
  (``mark_stale_sectors()``).  Every sector of a named column is marked, even
  when one of them had already failed for another reason: counting the column
  as failed but marking only that one sector used to send the column's other
  vertical stripes to a single-parity rebuild that folded the stale content
  in;
* an ordinary read of an unchecksummed block that the record names is treated
  as a failed checksum (``btrfs_check_read_bio()``), so it is read again through
  the rebuild above.  Before this, such a read returned the old content as the
  file's, silently, with the record naming it on disk the whole time
  (``tools/testing/btrfs/uml/stale_read.sh``);
* the record read off the disk at mount is consulted from the first read on
  (``consult_pending`` in ``raid56-wib.h``), not only once a read-write
  recovery has taken it over.  It used to be read after the tree roots and
  ignored until the recovery, so a RAID6 with one device missing and a column
  of another left stale -- two erasures with the record, an erasure plus an
  unlocated error without it -- failed to mount, and a read-only mount never
  consulted it at all (``tools/testing/btrfs/uml/early_record.sh``).

All of them sit behind one lock-free check that anything at all is recorded
stale, and that check counts a parity recorded as not describing the data as
well as a stale data column.  It used to count only the data, so a stripe whose
only record was a bad parity was invisible to every reader, and a degraded read
of an unchecksummed block rebuilt it out of that parity.

What retires a record
=====================

A write that a device did not take leaves a data column stale and names it in
the record.  Four things put that column back and restore the stripe's
redundancy:

* **the repair queued by the failed write itself.**  ``rmw_rbio()`` hands the
  full stripe to ``btrfs_raid56_queue_repair()``, and after a short delay
  (1s, doubling per attempt, six attempts) a *repair rbio* is submitted for it:
  a read-modify-write with no data of its own, which reads the stripe, rebuilds
  what the record proves stale and writes that and the parity back.  It queues
  on the same stripe lock as every write, so no write can use the stripe
  between the repair reading it and the repair landing.  A block group that is
  read-only belongs to a scrub, balance or replace and is left to it.  The
  queue is stopped and drained before the filesystem stops being writable
  (``btrfs_raid56_stop_repairs()`` at unmount and remount read-only).
  Bounded: 64 stripes waiting; what does not fit, or does not succeed, stays
  recorded for the next item on this list.
* **the next write to the stripe.**  A read-modify-write has always read the
  whole stripe and rebuilt what it could not believe, then dropped the rebuilt
  sectors after folding them into the new parity.  It now writes back every
  sector something *proved* wrong -- named by the record (and already checked
  by ``mark_stale_sectors()`` to fit the parity that is left), or rebuilt to a
  match against a checksum the on-disk copy failed -- along with the parity of
  every vertical stripe it touched that way (``rmw_prepare_repair()``).  A
  sector that was merely unreadable and has no checksum is *not* written back:
  its rebuild is a guess, and writing a guess over a sector whose read failed
  transiently is how good data gets destroyed (``uml/unprovable.sh``).  Once
  every sector of a named column is written or verified, its mark is cleared.
* **recovery**, at mount, on a ``remount,rw`` from read-only, and after a tree
  log replay, which repairs from the record exactly as a scrub does (see
  Recovery above);
* **any** ``btrfs scrub``, or a device replace, which drives the scrub
  machinery over the source device's stripes.

A write into a stripe the record *cannot* decide -- it names more members than
the parity that is left can rebuild -- is **refused** with ``-EIO``
(``rmw_refused`` in ``raid56_write_profile``).  Any parity it computed would
have to take the stale column as it is on disk, destroying the only copy of
what was acknowledged there, silently where there is no checksum.  Refusing
costs availability for that one stripe and nothing else: reads are unchanged,
and a stripe that became undecidable because a device is missing becomes
decidable again when the device returns.  The stale mark on a *parity* is now
also only cleared when the whole parity column was rewritten; a sub-stripe
write that landed used to clear the mark of a parity write that failed in a
different vertical stripe.

A device reappearing still triggers nothing by itself (it has no hook on a
mounted filesystem); what brings the stripe back is whichever of the above
comes first.

``tools/testing/btrfs/uml/rmw_repair.sh`` tests the three new paths, each
against a negative control that restores the old behaviour
(``raid56_rmw_no_repair``, ``raid56_rmw_no_refuse``,
``raid56_no_repair_on_fault``): the next write leaves 0 blocks stale on the
platters against 8; the refused writes lose 0 blocks from the parity against
8; with nothing writing at all, the queued repair leaves 0 stale against 8.

Verification
============

* ``tools/testing/btrfs/raid56_write_hole_model.py`` is an exhaustive state
  machine model of one vertical stripe under RMW: every combination of
  completed/lost/torn data and parity writes at the crash, every crash point
  of the protocol, and every loss of up to one (RAID5) or two (RAID6) devices
  afterwards.  It shows the loss of committed data without the log and none
  with it, for two to five data stripes.
* ``tools/testing/btrfs/raid56_wib_log_model.py`` is an exhaustive model of
  the on-disk protocol: log writes and flushes that fail per device, torn
  log writes at the crash, and device loss after it.  It shows that every
  stripe with unflushed or stale data on a surviving device is listed in the
  newest valid block of a surviving device, and that weakening the protocol
  (taking the set to write after the flush, not adding later log writes to
  the commit's snapshot, or dropping without a confirmed flush) breaks
  that.
* ``fs/btrfs/tests/raid56-wib-tests.c`` unit tests the in-memory tracking,
  the block encoding, torn/foreign block rejection, the recovery list merge,
  the log-full handling and the enable/disable ordering
  (``CONFIG_BTRFS_FS_RUN_SANITY_TESTS``).
* With ``CONFIG_BTRFS_DEBUG`` the module parameter
  ``btrfs.raid56_crash_point`` injects a crash into the next recorded write:
  1 drops the P/Q writes, 2 drops the data writes, then the kernel panics once
  the remaining writes completed; 4 panics the next parity scrub (the log
  replay) before its parity write.  Mounting afterwards and reading the
  previously committed data with any one device missing demonstrates the
  problem on a kernel without the log and its absence with it.
* ``tools/testing/btrfs/raid56_wib_dump.py`` prints the log blocks of a
  device or image.
