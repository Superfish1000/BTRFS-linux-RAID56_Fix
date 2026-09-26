/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RAID56 write-intent log ("wib": write-intent bitmap).
 *
 * See raid56-wib.c for the design and the crash-consistency argument.
 */

#ifndef BTRFS_RAID56_WIB_H
#define BTRFS_RAID56_WIB_H

#include <linux/types.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>
#include <uapi/linux/btrfs_tree.h>
#include <uapi/linux/btrfs.h>

struct btrfs_fs_info;
struct btrfs_io_context;

/*
 * What a user has to be told about.  See btrfs_raid56_alert().
 *
 * STALE is a notice: a device failed a write the parity covered, so that
 * stripe has lost its redundancy until it is repaired.  Every other event
 * means a write failed, a repair was abandoned or the log lost track of
 * something -- the filesystem is FAILING until nothing is recorded any more.
 */
enum btrfs_raid56_event {
	BTRFS_RAID56_EV_STALE,		/* a device failed a write within tolerance */
	BTRFS_RAID56_EV_REFUSED,	/* write refused: the repair did not land */
	BTRFS_RAID56_EV_NOT_DURABLE,	/* write refused: the repair could not be made durable */
	BTRFS_RAID56_EV_UNDECIDABLE,	/* write refused: more stale than the parity can rebuild */
	BTRFS_RAID56_EV_FAILED,		/* write failed on more devices than the parity covers */
	BTRFS_RAID56_EV_GAVE_UP,	/* a queued repair was abandoned */
	BTRFS_RAID56_EV_LOG_FULL,	/* write failed: the log stayed full */
	BTRFS_RAID56_EV_DROPPED,	/* the log was full and dropped a record */
	BTRFS_RAID56_EV_READ_AMBIGUOUS,	/* read refused: its rebuild needed a stale column */
	BTRFS_RAID56_EV_LOG_WRITE,	/* write failed: the log could not be written */
	BTRFS_RAID56_EV_REPAIR_DROPPED,	/* the repair queue was full: no automatic repair */
	BTRFS_RAID56_EV_READ_PARITY,	/* read refused: the RAID6 parities disagree */
	BTRFS_RAID56_EV_LOG_UNFLUSHED,	/* a failed flush left more to name than the log holds */
	BTRFS_RAID56_EV_REPLACE_LOST,	/* a replace could neither copy nor rebuild a sector */
	BTRFS_RAID56_EV_REPLACE_DROPPED, /* a full log dropped a replace's record: it fails */
	BTRFS_RAID56_EV_REPLACE_ABORTED, /* a replace could not record what it lost: it fails */
	BTRFS_RAID56_EV_READ_UNRECOVERED, /* read refused: its parity may be torn, unchecked */
	BTRFS_RAID56_EV_TORN_UNDECIDABLE, /* a torn stripe the recovery or a scrub cannot decide */
	BTRFS_RAID56_NR_EVENTS
};

enum btrfs_raid56_health {
	BTRFS_RAID56_HEALTH_OK,
	BTRFS_RAID56_HEALTH_DEGRADED,	/* something is recorded stale or waiting */
	BTRFS_RAID56_HEALTH_FAILING,	/* writes are failing or records were lost */
};

/* Devices named in alerts since the filesystem was last healthy. */
#define BTRFS_RAID56_ALERT_DEVS		8
#define BTRFS_RAID56_ALERT_NAME		64

struct btrfs_raid56_alert_dev {
	u64 devid;
	u64 events;
	char name[BTRFS_RAID56_ALERT_NAME];
};

/*
 * On-disk layout.
 *
 * Every device carries BTRFS_WIB_NR_SLOTS log blocks of BTRFS_WIB_SLOT_SIZE
 * bytes each, starting at physical offset BTRFS_WIB_OFFSET.  The offset lies
 * inside the first megabyte of the device, which btrfs never hands out to
 * chunks (BTRFS_DEVICE_RANGE_RESERVED) and which only contains the primary
 * superblock at 64KiB.
 *
 * The two slots of a device are written alternately so that a torn write of
 * one slot always leaves the other, older slot intact.  A slot is valid when
 * its magic, fsid and checksum match; recovery uses the union of the newest
 * valid slot of every present device, so any valid slot can only add stripes
 * to recover, never hide one.
 */
#define BTRFS_WIB_OFFSET		SZ_512K
#define BTRFS_WIB_SLOT_SIZE		SZ_4K
#define BTRFS_WIB_NR_SLOTS		2
/*
 * How long btrfs_wib_mark() waits for a full log to drain before failing the
 * write.  Generous: the RMWs that free entries only have to complete their
 * own device IO, so reaching this means something is genuinely stuck.
 */
#define BTRFS_WIB_FULL_TIMEOUT		(60 * HZ)

/* "RI56_WIL" in little endian. */
#define BTRFS_WIB_MAGIC			0x4c49575f36354952ULL

/*
 * Granularity of the log: one bit covers BTRFS_STRIPE_LEN (64KiB) of logical
 * address space, one entry covers 64 such blocks (4MiB), naturally aligned.
 * A full stripe (nr_data * 64KiB, aligned to its own length) therefore maps
 * to nr_data consecutive bits, possibly straddling two entries.
 */
#define BTRFS_WIB_BLOCK_SHIFT		16
#define BTRFS_WIB_BLOCK_SIZE		(1ULL << BTRFS_WIB_BLOCK_SHIFT)
#define BTRFS_WIB_ENTRY_SHIFT		(BTRFS_WIB_BLOCK_SHIFT + 6)
#define BTRFS_WIB_ENTRY_SIZE		(1ULL << BTRFS_WIB_ENTRY_SHIFT)

/*
 * Format 1: no stale record.  Written by earlier versions of this feature,
 * read here so that upgrading does not throw a log away -- ignoring one
 * leaves the stripes it covers unrecovered, which is the whole thing the
 * feature exists to prevent.  Never written.
 */
struct btrfs_wib_disk_entry_v1 {
	__le64 bytenr;
	__le64 bitmap;
	__le64 error;
} __packed;

struct btrfs_wib_disk_entry {
	/* BTRFS_WIB_ENTRY_SIZE aligned logical address. */
	__le64 bytenr;
	/* Bit i set: [bytenr + i * 64K, +64K) has a sub-stripe write in flight. */
	__le64 bitmap;
	/*
	 * Bit i set: a write to that block failed on some device, or the
	 * block could not be fully recovered at the last mount.  Recovery
	 * verifies such a stripe but does not trust its unverifiable
	 * sectors (see btrfs_wib_recover()).
	 */
	__le64 error;
	/*
	 * Bit i set: the DATA of that block on disk is not what was
	 * acknowledged -- the acknowledged value survives only in the parity.
	 * A strict subset of @error.
	 *
	 * @error says a write went wrong; it does not say which SIDE of the
	 * stripe is wrong, and the two need opposite repairs.  If the data is
	 * stale, recomputing the parity from it destroys the last copy of what
	 * was acknowledged; if only the parity is stale, recomputing it from
	 * the data is exactly the right repair.  For nodatacow data there is
	 * no checksum to tell them apart, so this record is the only thing
	 * that can.
	 *
	 * Persisted because the protection is worth nothing otherwise: an
	 * exhaustive model of the scrub decision
	 * (tools/testing/btrfs/scrub_policy_model.py) scores the scrub fix at
	 * 0 stripes destroyed with this record available and 1488 of 8386
	 * RAID5 states destroyed without it, which is upstream's own score.
	 */
	__le64 stale;
	/*
	 * Bit (b + p), where b is the block at a full stripe's start: parity p
	 * of that stripe does not describe the data on disk.  nr_data is at
	 * least 2 for every RAID5/6 chunk, so those bits always belong to the
	 * stripe they describe and never to the next one.
	 *
	 * Needed alongside @stale rather than folded into it: rebuilding a
	 * stale data column out of a parity that is itself stale returns a
	 * value that was never committed anywhere, which is how a record meant
	 * to protect data ends up destroying it.
	 */
	__le64 stale_par;
	/*
	 * The newest filesystem generation at which any block of this region
	 * gained a fault record.
	 *
	 * Everything else a recovery helper needs can be recovered by reading
	 * the disks later: the geometry and the column-to-device mapping from
	 * the chunk tree, the file behind an address from
	 * BTRFS_IOC_LOGICAL_INO, and both candidate values for a named column
	 * from the devices themselves.  This cannot.  A logical address is
	 * reused once its extent is freed and reallocated, so without knowing
	 * WHEN the damage was recorded a helper cannot tell whether the extent
	 * it finds there now is the one that was damaged -- and pointing a
	 * human at the wrong file is worse than pointing them at none.
	 *
	 * An upper bound over the region rather than a per-block value, so the
	 * test it exists for stays conservative: an extent newer than this was
	 * written after the record and the record does not describe it.
	 */
	__le64 gen;
} __packed;

struct btrfs_wib_disk_header {
	/* Checksum (fs csum type) of the block starting after this field. */
	u8 csum[BTRFS_CSUM_SIZE];
	/* metadata_uuid of the filesystem this block belongs to. */
	u8 fsid[BTRFS_FSID_SIZE];
	__le64 magic;
	/* Monotonically increasing per commit. */
	__le64 seq;
	__le32 nr_entries;
	/* BTRFS_WIB_BLOCK_SHIFT of the writer, for future format changes. */
	__le32 block_shift;
	__le64 flags;
	__le64 reserved[6];
} __packed;

#define BTRFS_WIB_MAX_ENTRIES						\
	((BTRFS_WIB_SLOT_SIZE - sizeof(struct btrfs_wib_disk_header)) /	\
	 sizeof(struct btrfs_wib_disk_entry))
#define BTRFS_WIB_MAX_ENTRIES_V1					\
	((BTRFS_WIB_SLOT_SIZE - sizeof(struct btrfs_wib_disk_header)) /	\
	 sizeof(struct btrfs_wib_disk_entry_v1))
/*
 * Size of the in-memory table, and the bound for every loop over it.  Kept
 * separate from the two on-disk maxima above because it answers a different
 * question: how many regions a mount can track, which has nothing to do with
 * how wide an entry has to be on disk.  It is the larger of the two, so that a
 * filesystem which has never had a stale record can use all of it.
 *
 * Being the larger is exactly why the table alone is not the bound.  A wide
 * block describes only BTRFS_WIB_MAX_ENTRIES regions, so a full table cannot
 * be written once anything is stale; wib_live_max() is the live bound, and
 * wib_enforce_capacity_locked() restores it when a stale bit halves it.
 */
#define BTRFS_WIB_NR_ENTRIES		BTRFS_WIB_MAX_ENTRIES_V1

/*
 * Header flags.  btrfs_wib_block_valid() rejects a block carrying any bit not
 * listed here: an unknown bit means a format this kernel cannot read, and
 * guessing at one could scrub the wrong stripes or silently skip the right
 * ones.
 */
#define BTRFS_WIB_FLAG_STALE		(1ULL << 0)
#define BTRFS_WIB_FLAGS_SUPPORTED	BTRFS_WIB_FLAG_STALE

/*
 * The last eight bytes of a slot.  They lie past the entries of either layout
 * (165 narrow ones leave 8 bytes, 82 wide ones 32) and inside the checksum.  A
 * block whose writer marks the records that may hide a torn write (@torn in
 * struct btrfs_wib_entry) carries BTRFS_WIB_TORN_MARKING there; one that does
 * not may list such a write as a plain error record, and btrfs_wib_load()
 * reads every error record of it as possibly torn.
 *
 * Not a header flag, nor a reserved field: btrfs_wib_block_valid() refuses
 * both when it does not know them, in every kernel that has the log, and a
 * refused log is one whose stripes are never recovered.  The kernels from
 * before the mark zero the whole slot when they build a block and never look
 * at these bytes, so they neither write the marker nor refuse a block that
 * has it.
 */
#define BTRFS_WIB_TRAILER_OFFSET	(BTRFS_WIB_SLOT_SIZE - sizeof(__le64))
/* "TORNMARK" in little endian. */
#define BTRFS_WIB_TORN_MARKING		0x4b52414d4e524f54ULL

/* In-memory entry, mirrors the on-disk one. */
struct btrfs_wib_entry {
	u64 bytenr;
	/* Blocks with a sub-stripe write in flight. */
	u64 bitmap;
	/*
	 * Blocks whose sub-stripe write completed with a device error, whose
	 * log record may not have reached a device with its data flushed, or
	 * whose recovery could not complete: the stripe may be inconsistent
	 * on some device without any crash, keep it logged so that it is
	 * scrubbed at the next mount.  Cleared by a successful recovery, or
	 * by eviction when the log is full.
	 */
	u64 sticky;
	/*
	 * Blocks whose data a failed write left stale on disk: the value that
	 * was acknowledged survives only in the parity.  A checksum would say
	 * the same thing about the sector, and for nodatacow data this is the
	 * only thing that can: without it the next read-modify-write of the
	 * same full stripe reads the stale sector, believes it, and computes a
	 * parity from it -- destroying the copy that still had the
	 * acknowledged content.  See btrfs_wib_stale() and
	 * verify_bio_data_sectors().
	 *
	 * A strict subset of @sticky, which btrfs_wib_block_valid() enforces
	 * on read.  Persisted, as @stale in the on-disk entry: across a mount
	 * @sticky alone says a write went wrong without saying which side of
	 * the stripe is wrong, and a scrub that cannot tell recomputes the
	 * parity from the stale sector.
	 */
	u64 stale;
	/*
	 * Which PARITY of a full stripe a failed write left not describing the
	 * data.  Bit (b + p), where b is the block of the full stripe's start,
	 * means parity p of that stripe is stale.  nr_parity is at most 2 and
	 * nr_data at least 2, so those bits always belong to the stripe they
	 * describe and never to the next one; a stripe straddling two entries
	 * is handled by addressing them logically, like every other block.
	 *
	 * A stale parity is not a copy of anything.  Rebuilding a data column
	 * from one hands back a value that was never committed anywhere, which
	 * is how a record meant to protect data ends up destroying it -- see
	 * the counterexamples in tools/testing/btrfs/scrub_policy_model.py.
	 * Kept in its own field rather than folded into @stale so that the two
	 * questions stay separable: which side of the stripe is wrong is the
	 * whole decision.
	 *
	 * Persisted alongside @stale, for the same reason.
	 */
	u64 stale_par;
	/* See @gen in the on-disk entry. */
	u64 gen;
	/*
	 * In memory only.  The @stale (@hold) and @stale_par (@hold_par) bits
	 * a failed flush named while the block had a write in flight
	 * (wib_readd_dropped()).  That write may have landed part of itself
	 * on the device before the flush failed, into the cache the device
	 * then lost, and when it completes it clears the marks of every
	 * column and parity it wrote (rmw_update_stale_data(),
	 * rmw_update_stale_parity()) -- the very marks that say it may not
	 * be there.  So those clears leave these bits alone.  A later write
	 * of the stripe starts after the failed flush and releases them
	 * (btrfs_wib_try_mark()); so does a write-back with FUA, which no
	 * lost cache can take away (btrfs_wib_clear_stale(@durable)).
	 */
	u64 hold;
	u64 hold_par;
	/*
	 * In memory only.  The @stale (@replace_stale) and @stale_par
	 * (@replace_stale_par) bits a running device replace set for the
	 * zeros it put on its target where it could neither copy nor rebuild
	 * (btrfs_wib_replace_mark_stale(), btrfs_wib_replace_mark_parity()),
	 * and that nothing else has set since.  They describe the target only,
	 * which nothing reads until the replace finishes: until then the
	 * queries leave them out (btrfs_wib_stale(), btrfs_wib_stripe_state()),
	 * so the source goes on serving the column as it did.  A finished
	 * replace hands them over as ordinary marks, an aborted one drops them
	 * with its target (btrfs_wib_replace_end()).  Any other writer of the
	 * same bit takes it over (wib_replace_disown()): that mark is about the
	 * source too, and must be neither hidden nor dropped.
	 */
	u64 replace_stale;
	u64 replace_stale_par;
	/*
	 * In memory only.  Every @stale (@replace_keep) and @stale_par
	 * (@replace_keep_par) bit a running device replace recorded zeros on
	 * its target under, the ones that were stale already included: a
	 * column a degraded write named stale is the usual case, and
	 * @replace_stale leaves it out, since that mark is not the replace's
	 * to hide or drop.  The zeros are there all the same, and once the
	 * target takes over only the mark says so.  So a full log spends these
	 * as the replace's own (wib_entry_replace_owned()), and another writer
	 * of the bit does not take them over: what it says about the source
	 * does not take the zeros off the target.  A write that puts the
	 * column right does, on the target too, and clears the bit with the
	 * mark (btrfs_wib_clear_stale()); the rest go when the replace ends
	 * (btrfs_wib_replace_end()).
	 */
	u64 replace_keep;
	u64 replace_keep_par;
	/*
	 * Blocks of @sticky whose record may hide a torn write: a write went
	 * to the member of a device that then failed a flush, and the record
	 * could not name it (wib_readd_dropped() with no room for the names),
	 * or the record was loaded with a write in flight and the mount could
	 * not resolve it (btrfs_wib_recover()).  Such a stripe's parity may
	 * not describe its data, which is what the rules for a write in
	 * flight at mount exist for -- and without this a plain record, made
	 * of in-flight bits the readd turned sticky, reads to the next mount
	 * like a plain failed write, and gets none of them.
	 *
	 * Written to disk as in flight (@bitmap | @torn in the on-disk entry),
	 * which costs no room and no format change, and which a kernel that
	 * does not know the mark reads the conservative way.  A block from a
	 * kernel that does not mark has no BTRFS_WIB_TORN_MARKING, and every
	 * error record of it is loaded with @torn set (btrfs_wib_load_block()):
	 * in @pending only, this field says that.  Kept here apart
	 * from @bitmap, which means a write that will complete: in @bitmap it
	 * would hold off eviction as a write in flight does, make a full log
	 * wait for it, and be cleared by btrfs_wib_done().  Cleared only with
	 * @sticky, where the stripe is rewritten or found consistent.
	 */
	u64 torn;
	/*
	 * In memory only.  The @torn bits the mount's recovery kept because it
	 * could not decide the stripe (wib_keep_torn()): a sector there does
	 * not read, say, so the refusal the mark stands for (recover_rbio()) is
	 * in use now, where one a failed flush left matters only if a sector
	 * fails later.  A full log with every device there spends them last of
	 * the records that say no more than that a write may have been torn
	 * (wib_evict_sticky()).  Cleared with @torn.
	 */
	u64 kept_torn;
	/*
	 * In memory only.  The @stale_par bits that are the mount's verdict on
	 * a possibly torn stripe with a data column on a missing device, which
	 * it could not decide (btrfs_wib_mark_suspect_parity()), and that
	 * nothing else has set since.  In memory they are stale parities like
	 * any other: a read of the column's unchecksummed sectors fails, a
	 * write into the stripe is refused.  On disk they need not be: the
	 * stripe is marked possibly torn, and the next mount reaches the same
	 * verdict from that mark while the device is missing, or regenerates
	 * the parity from the data once it is back, as it would with the
	 * parities named.  So they do not make a block wide by themselves
	 * (wib_entry_wide()); a block wide for another reason carries them.
	 * A full log with a device missing spends them last
	 * (wib_evict_sticky()).  Any other writer of the bit takes it over
	 * (wib_replace_disown()).
	 */
	u64 suspect_par;
	/*
	 * In memory only.  The @stale_par bits a recovery found recorded on a
	 * possibly torn stripe with a data column on a missing device, which
	 * leave it more unknown columns than usable parities
	 * (btrfs_wib_keep_prior_verdict()): an earlier mount's verdict, which
	 * a block wide for another reason carried as the stale parity it is
	 * in memory and which reloads as one -- or a parity a write left
	 * stale; the block cannot say which.  This mount cannot reach the
	 * verdict again (the stale parity keeps the recovery from classifying
	 * the stripe), and either way the mark is all that keeps a read of the
	 * column's unchecksummed data from a rebuild out of that parity.  So a
	 * full log with a device missing spends it last, as a verdict
	 * (wib_entry_verdict()); unlike @suspect_par it counts as stale for the
	 * layout (wib_entry_wide()), since a stale parity must reach the next
	 * mount named.  Any other writer of the bit takes it over.
	 */
	u64 prior_par;
};

/* Full stripes that can wait for a repair at once; see btrfs_raid56_queue_repair(). */
#define BTRFS_WIB_REPAIR_SLOTS		64

struct btrfs_wib_repair_slot {
	u64 logical;
	unsigned long due;
	u8 tries;
};

/*
 * The devices that did not confirm the cache flush a drop from the log was
 * going to rely on: the transaction commit's barrier (BTRFS_DEV_STATE_FLUSH_FAILED)
 * or the log's own flush (wib_flush_all_devices()).  See wib_readd_dropped().
 */
#define BTRFS_WIB_FLUSH_FAILED_MAX	16

struct btrfs_wib_flush_failed {
	unsigned int nr;
	/* More failed than @devid holds: every device counts as failed. */
	bool overflow;
	u64 devid[BTRFS_WIB_FLUSH_FAILED_MAX];
};

/*
 * What wib_name_devices() found for one entry of the last block: the blocks
 * whose data column (@stale) or parity (@stale_par, addressed as the record
 * addresses it) lies on a device that did not confirm a flush, and those of
 * them on a device the mount could not find (@absent).  @repair: the blocks
 * the readd then named, on a device that is there, whose full stripes it asks
 * to have repaired (wib_readd_queue_repairs()).  @landed: the blocks the entry
 * lists in flight whose write finished before the failed flush was issued,
 * and which the readd took back as the error record that says what the flush
 * may have lost (wib_readd_base()).
 */
struct btrfs_wib_names {
	u64 stale;
	u64 stale_par;
	u64 absent;
	u64 repair;
	u64 landed;
};

/*
 * In memory only.  Which members of the blocks of one region have had a write
 * without FUA issued to them since the device holding the member last
 * confirmed a cache flush: @cols bit b for the data column of block b, @par
 * bit (b + p) for parity p of the full stripe starting at block b, the way
 * the record addresses them.  A write that failed, or that was skipped for a
 * missing device, counts; a write-back with FUA does not.
 *
 * A device that fails a flush may have lost exactly these writes from its
 * cache, and nothing else: a member it confirmed a flush for since, or that no
 * write went to, holds what it held.  So these are the only members a failed
 * flush names (wib_readd_dropped()).  Filled when the bios are issued
 * (btrfs_wib_note_written()), not when they complete -- a write in flight
 * during the failed flush is in the device's cache already -- and cleared by a
 * flush the device confirmed, for blocks with no write in flight when the
 * flush was issued, or once the failed flush has named the member.  Protected
 * by wib->lock.
 */
struct btrfs_wib_written {
	u64 bytenr;
	u64 cols;
	u64 par;
};

/*
 * Rows of written records.  A logged write notes a region the last block lists;
 * any other write, one the in-memory set records (btrfs_wib_note_written()).
 * Each lists at most BTRFS_WIB_NR_ENTRIES regions.
 */
#define BTRFS_WIB_WRITTEN_SLOTS		(2 * BTRFS_WIB_NR_ENTRIES)

/* A region's in-flight bits in a snapshot block, see wib_load_snapbits(). */
struct btrfs_wib_bits {
	u64 bytenr;
	u64 bits;
};

struct btrfs_wib {
	struct btrfs_fs_info *fs_info;

	/*
	 * Full stripes a write that hit a device error asked to have repaired,
	 * drained by btrfs_raid56_repair_work() (raid56.c).  Protected by
	 * @repair_lock.  @repairs_inflight counts the repair rbios submitted
	 * and not yet finished; teardown waits for it on @wait.
	 */
	spinlock_t repair_lock;
	struct delayed_work repair_work;
	bool repair_stopped;
	/* Frozen filesystem: keep the queue, submit nothing. */
	bool repair_paused;
	unsigned int repair_nr;
	struct btrfs_wib_repair_slot repair_queue[BTRFS_WIB_REPAIR_SLOTS];
	atomic_t repairs_inflight;
	atomic64_t stat_repair_queued;
	atomic64_t stat_repair_ok;
	atomic64_t stat_repair_failed;
	atomic64_t stat_repair_dropped;
	atomic64_t stat_repair_skipped;

	/*
	 * Protects entries[], snap_seq, seq, enabled and the enable/disable
	 * requests.  Never held across IO.
	 */
	spinlock_t lock;

	/*
	 * Serializes commits (the on-disk writes).  Also protects block,
	 * last and last_ok.
	 */
	struct mutex commit_mutex;

	/*
	 * Woken when an entry becomes free (all its in-flight bits cleared)
	 * and when a commit completes.
	 */
	wait_queue_head_t wait;
	/*
	 * Under @lock: whether a full log may spend a record naming a stale
	 * member, taken once per locked section (wib_policy_locked()) so the
	 * room counted and the room found by eviction always agree.
	 */
	bool may_evict_naming;
	/*
	 * Under @lock: a full log spent a record that holds a running device
	 * replace's own marks (@replace_keep in struct btrfs_wib_entry), so
	 * the replace's target holds zeros nothing records.  The replace must
	 * not finish (btrfs_wib_replace_marks_lost()); cleared when it ends.
	 */
	bool replace_marks_lost;

	/* Sequence number of the last successful commit. */
	u64 seq;
	/* Sequence number of the last snapshot taken (>= seq). */
	u64 snap_seq;

	/*
	 * True once the log is being persisted.  Marks are always tracked in
	 * memory so that enabling the log at runtime persists everything
	 * that is in flight at that moment.
	 */
	bool enabled;
	/* Enable at the next transaction commit (set without locks held). */
	bool enable_requested;
	/*
	 * Stop persisting once the superblock without the feature flag is
	 * durable: set by the request, armed by the commit that writes such
	 * a superblock, acted upon by the following one.
	 */
	bool disable_requested;
	bool disable_armed;
	/*
	 * A commit sampled enable_requested and is writing the log out.  The
	 * log is not enabled yet, so a disable arriving now cannot express
	 * itself through disable_requested the usual way; this tells it to
	 * do so anyway, and tells the commit not to set the feature flag.
	 */
	bool enable_in_progress;

	/*
	 * How many blocks across all entries currently carry a stale record.
	 * btrfs_wib_stale() is asked about EVERY sector that has no checksum,
	 * which on a nodatacow filesystem is every sector it reads, so the
	 * common answer has to be free of both the lock and the table walk.
	 * Stale records only exist after a device error, so this is normally
	 * zero and the query never touches wib->lock.
	 */
	atomic_t nr_stale;

	/* In-flight sub-stripe writes, bitmap == 0 means the entry is free. */
	/*
	 * Sized by the LARGER of the two on-disk layouts.  The live table's
	 * capacity has nothing to do with how wide an entry has to be on
	 * disk, and letting the narrower format set it would shrink what a
	 * mount can track for no reason at all.
	 */
	struct btrfs_wib_entry entries[BTRFS_WIB_NR_ENTRIES];

	/* Page sized buffer holding the block being written. */
	void *block;
	/*
	 * Copy of the last block built for a commit, written or not.  Every
	 * device holds this block or an older one that lists a subset of it.
	 */
	void *last;
	/* The last block reached every device it was written to. */
	bool last_ok;
	/*
	 * Snapshot of the set taken before the devices were flushed; a
	 * commit after the flush drops only what finished before it.
	 */
	void *prepared;
	bool prepared_valid;
	/*
	 * The same, for the flush-and-drop path, which takes its own snapshot
	 * before flushing.  It needs a buffer of its own: wib->block is the
	 * scratch that block building writes into, and @prepared belongs to
	 * the transaction commit.
	 */
	void *flushsnap;
	/*
	 * Under @commit_mutex.  A device did not confirm a flush, and the
	 * block that would hold the in-memory set together with every record
	 * of the last block cannot be written yet: writes in flight that the
	 * last block does not list take the room, and finishing makes it
	 * (wib_readd_dropped()).  The set is all a later drop keeps, so until
	 * it has taken the records back no commit drops anything from the
	 * last block -- not even after a flush every device confirmed, which
	 * does not bring back what the failed one may have lost -- and
	 * @readd_owed_failed says which devices those stripes have to name
	 * when it does.  Never a state the log cannot leave: no write takes
	 * more of the room meanwhile (@readd_admit_last), and when finishing
	 * writes cannot make it, the readd does what fits instead.
	 */
	bool readd_owed;
	struct btrfs_wib_flush_failed readd_owed_failed;
	/*
	 * Under @lock, set with @readd_owed (wib_readd_set_owed()).  Every
	 * write into a region the last block does not list adds one more that
	 * the block after the readd has to describe, and while such writes
	 * keep arriving the readd never finds its room: no block is written,
	 * and every recorded write fails.  So while it is owed, btrfs_wib_mark()
	 * records no write into a region that the last block does not list --
	 * @readd_last, its regions sorted, @nr_readd_last of them -- unless the
	 * set holds a record of it (wib_readd_refuses_locked()), but plans the
	 * readd again once the writes holding the room are gone
	 * (wib_readd_settle()).  At @readd_until the readd stops waiting for
	 * them, and takes back what fits instead.
	 */
	bool readd_admit_last;
	u32 nr_readd_last;
	u64 *readd_last;
	unsigned long readd_until;
	/* Under @commit_mutex: who failed the flush just issued. */
	struct btrfs_wib_flush_failed flush_failed;
	/*
	 * Under @commit_mutex: wib_name_devices() for each entry of @last,
	 * worked out before wib_readd_dropped() takes @lock -- the chunk map
	 * is not walked with interrupts off -- and kept here so that a commit
	 * allocates nothing.
	 */
	struct btrfs_wib_names readd_names[BTRFS_WIB_NR_ENTRIES];
	/*
	 * Under @commit_mutex.  @readd_landed: the last wib_readd_dropped()
	 * took the records of @last back and worked out @landed in
	 * @readd_names for them.  @readd_base: @last as the block written
	 * after that readd unions it, see wib_readd_base().
	 */
	bool readd_landed;
	void *readd_base;
	/*
	 * Under @commit_mutex: saying an owed readd's wait, see
	 * wib_readd_say_wait().  @readd_said: the one owed now was said.
	 */
	struct ratelimit_state readd_say_rs;
	u32 readd_unsaid;
	bool readd_said;
	/*
	 * BTRFS_WIB_WRITTEN_SLOTS of them, see struct btrfs_wib_written.  Every
	 * region with a member written lies in @last or in the in-memory set,
	 * so they fit; were one ever not to, @written_unknown makes every
	 * member count as written -- the naming as it was before these were
	 * kept -- until a flush every device confirmed leaves @last empty.
	 * Only kept while the log is enabled: nothing names anything otherwise.
	 * A disable forgets them once the last block is written, not before
	 * (wib_write_final_locked()), and a readd still owed then names every
	 * member when the log is enabled again (wib_written_reset_locked()).
	 */
	struct btrfs_wib_written *written;
	bool written_unknown;
	/*
	 * Under @commit_mutex: the in-flight bits of the snapshot a flush was
	 * issued after, sorted by region (wib_load_snapbits()), and how many.
	 * BTRFS_WIB_NR_ENTRIES of them, allocated with the log.
	 */
	struct btrfs_wib_bits *snapbits;
	u32 nr_snapbits;

	/* IO completion tracking for one commit, commit_mutex held. */
	atomic_t io_pending;
	wait_queue_head_t io_wait;

	/* Dirty regions loaded from disk at mount, waiting for recovery. */
	struct btrfs_wib_entry *pending;
	unsigned int nr_pending;
	unsigned int max_pending;
	/*
	 * Until the first read-write recovery takes the record over, the stale
	 * marks read off the disk exist only in @pending, and every read before
	 * that -- the tree roots at mount, everything on a read-only mount --
	 * would be answered as if they did not exist.  While this is set the
	 * staleness queries look in @pending as well, and @nr_pending_stale of
	 * wib->nr_stale is theirs.  Protected by wib->lock.
	 */
	bool consult_pending;
	unsigned int nr_pending_stale;
	/*
	 * @pending holds records no recovery has taken over yet: every write
	 * they list may have been torn, and until a read-write mount recovers
	 * them nothing has checked a parity of theirs.  A read-only mount
	 * never does, so it answers btrfs_wib_unrecovered() from them for as
	 * long as it lasts.  Set at load, cleared with @consult_pending once a
	 * recovery has taken every stripe over; protected by wib->lock.
	 */
	bool pending_unrecovered;
	/*
	 * One per @pending entry: the blocks of it a recovery has taken over,
	 * a full stripe at a time, just before it recovers the stripe
	 * (btrfs_wib_take_pending()).  The live table answers for those from
	 * then on, so the queries above leave them out -- and go on answering
	 * from @pending for every stripe the recovery has not reached, while
	 * it runs on a mount that is still read-only and serving reads, and
	 * for good if it stops early and the remount fails.  Allocated by the
	 * first recovery, freed with @pending; protected by wib->lock.
	 */
	u64 *pending_taken;
	/*
	 * The full stripe [@recovering, @recovering + @recovering_len) the
	 * recovery has taken over and is deciding right now, when a write into
	 * it may have been torn; @recovering_len 0 when none.  Between the take
	 * and the verdict neither @pending nor the live table answers for it:
	 * btrfs_wib_recovering() does.  Protected by wib->lock.
	 */
	u64 recovering;
	u64 recovering_len;

	/* Statistics, exported through sysfs. */
	/*
	 * How the staleness query was answered.  btrfs_wib_stale() is asked
	 * about every sector without a checksum, so on a nodatacow filesystem
	 * these count every sector an RMW reads: @stat_stale_fast is the
	 * lock-free answer, @stat_stale_slow the walk under wib->lock.  The
	 * ratio is the whole justification for wib->nr_stale.
	 */
	atomic64_t stat_stale_fast;
	atomic64_t stat_stale_slow;

	atomic64_t stat_marks;
	atomic64_t stat_commits;
	atomic64_t stat_commit_flushes;
	atomic64_t stat_recovered_stripes;
	atomic64_t stat_recovery_errors;
	/*
	 * Full stripes the mount recovery found with a write in flight and a
	 * data column on a missing device, could not decide, and so recorded
	 * every parity still there as stale: see scrub_raid56_recover_absent().
	 */
	atomic64_t stat_recovery_suspect;
	atomic64_t stat_sticky;
	atomic64_t stat_sticky_evicted;
	/*
	 * The subset of @stat_sticky_evicted that took a named member with it
	 * (@stale or @stale_par).  Worth its own counter because the two
	 * losses are not the same: a scrub can rediscover that something went
	 * wrong in a stripe by reading it, but nothing can work out again
	 * which member a write failed on.  Non-zero means evidence is gone,
	 * not merely that the log was busy.
	 */
	atomic64_t stat_stale_evicted;
	atomic64_t stat_commit_errors;
	/*
	 * Full stripes a scrub declined to regenerate the parity of, because
	 * the log records one of their data columns stale and the parity is
	 * the only place the acknowledged content still exists.
	 */
	atomic64_t stat_scrub_skipped_stale;
	/*
	 * Reads that found the log naming more members than the surviving
	 * parity can rebuild, and so returned the sectors as they are on disk
	 * rather than reconstructing a value nothing ever committed.
	 *
	 * The highest-frequency discoverer of an ambiguous stripe in the
	 * system, and until this counter existed it left no trace at all: the
	 * budget check in mark_stale_sectors() simply returned.  A scrub
	 * reports what it declined to repair; this reports what a read
	 * declined to trust.
	 */
	atomic64_t stat_read_ambiguous;

	/*
	 * Alerts: see btrfs_raid56_alert().  The counters are totals since
	 * mount; everything under @alert_lock describes the current episode,
	 * which ends when nothing is recorded stale or waiting any more.
	 */
	atomic64_t stat_alert[BTRFS_RAID56_NR_EVENTS];
	spinlock_t alert_lock;
	bool alert_stopped;
	/* Set by any event but STALE; cleared when the episode ends. */
	bool alert_failing;
	/*
	 * Events no record stands for -- see BTRFS_RAID56_LATCHED_EVENTS --
	 * that keep the episode open until someone acknowledges them.
	 */
	unsigned long alert_latched;
	/* Bumped by every latched event: "ack <seq>" names what was seen. */
	u64 alert_latch_seq;
	/* Something new to announce even if the state did not change. */
	bool alert_announce;
	/* Events whose first occurrence this episode has been explained. */
	unsigned long alert_seen;
	/* Events since the last summary. */
	u64 alert_pending[BTRFS_RAID56_NR_EVENTS];
	enum btrfs_raid56_health health;
	enum btrfs_raid56_event last_event;
	u64 last_logical;
	u64 last_devid;
	unsigned long last_jiffies;
	char last_name[BTRFS_RAID56_ALERT_NAME];
	struct btrfs_raid56_alert_dev alert_devs[BTRFS_RAID56_ALERT_DEVS];
	struct delayed_work alert_work;
};

void btrfs_raid56_alert(struct btrfs_fs_info *fs_info, enum btrfs_raid56_event ev,
			u64 logical, const struct btrfs_io_context *bioc,
			unsigned long cols);
ssize_t btrfs_raid56_health_show(struct btrfs_fs_info *fs_info, char *buf);
int btrfs_raid56_health_ack(struct btrfs_fs_info *fs_info, bool check_seq, u64 seq);
#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
bool btrfs_wib_evicts_naming(void);
bool btrfs_wib_keeps_naming_degraded(void);
bool btrfs_wib_readd_legacy(void);
bool btrfs_wib_name_unwritten(void);
bool btrfs_wib_all_records_torn(void);
bool btrfs_wib_torn_no_persist(void);
bool btrfs_wib_log_unmarked(void);
bool btrfs_wib_trust_unmarked_log(void);
bool btrfs_wib_missing_parity_keeps_torn(void);
bool btrfs_wib_absent_decided_keeps_torn(void);
bool btrfs_wib_suspect_as_stale(void);
bool btrfs_wib_replace_end_clears_verdicts(void);
bool btrfs_wib_evicts_replace_marks(void);
bool btrfs_wib_replace_keeps_added_only(void);
bool btrfs_wib_reload_verdicts_plain(void);
void btrfs_wib_take_pending(struct btrfs_wib *wib, u64 start, u64 len, bool take);
bool btrfs_wib_disable_forgets_writes(void);
bool btrfs_wib_snapshot_misses_marks(void);
bool btrfs_wib_readd_admits_new(void);
bool btrfs_wib_readd_no_repair(void);
bool btrfs_wib_finished_stay_inflight(void);
bool btrfs_wib_remount_ro_keeps_inflight(void);
bool btrfs_wib_readd_disowns_all(void);
bool btrfs_wib_readd_admits_busy(void);
bool btrfs_wib_torn_unevictable(void);
bool btrfs_wib_torn_spent_eagerly(void);
bool btrfs_wib_kept_torn_in_order(void);
#endif
#ifdef CONFIG_BTRFS_DEBUG
bool btrfs_wib_unrecovered_as_ambiguous(void);
bool btrfs_wib_torn_remedy_legacy(void);
#else
static inline bool btrfs_wib_unrecovered_as_ambiguous(void) { return false; }
static inline bool btrfs_wib_torn_remedy_legacy(void) { return false; }
#endif
void btrfs_raid56_alert_stop(struct btrfs_fs_info *fs_info);

int btrfs_wib_alloc(struct btrfs_fs_info *fs_info);
void btrfs_wib_free(struct btrfs_fs_info *fs_info);

int btrfs_wib_load(struct btrfs_fs_info *fs_info);
int btrfs_wib_recover(struct btrfs_fs_info *fs_info, bool log_replay_pending);
int btrfs_wib_recover_after_replay(struct btrfs_fs_info *fs_info);
int btrfs_wib_persist_now(struct btrfs_fs_info *fs_info);
int btrfs_wib_rw_mount(struct btrfs_fs_info *fs_info, bool log_replay_pending,
		       bool rdonly);
void btrfs_wib_ro_mount(struct btrfs_fs_info *fs_info);
bool btrfs_wib_unrecovered(struct btrfs_fs_info *fs_info, u64 full_stripe_start,
			   int nr_data);
bool btrfs_wib_stripe_torn(struct btrfs_fs_info *fs_info, u64 start, u64 len);
bool btrfs_wib_recovering(struct btrfs_fs_info *fs_info, u64 full_stripe_start);
/*
 * Besides the parities on a missing device (bits 0 and 1), what the recovery
 * found a full stripe consistent but for (btrfs_scrub_raid56_full_stripe()'s
 * @unwritten_par): a data column on a missing device it decided, both parities
 * there and regenerated.  See btrfs_wib_parity_unwritten().
 */
#define BTRFS_WIB_STRIPE_DECIDED	BIT(2)
#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
void btrfs_wib_parity_unwritten(struct btrfs_fs_info *fs_info, u64 start, u64 len,
				unsigned int parities);
#endif
void btrfs_wib_unmount(struct btrfs_fs_info *fs_info);
void btrfs_wib_remount_ro(struct btrfs_fs_info *fs_info);

int btrfs_wib_enable(struct btrfs_fs_info *fs_info);
void btrfs_wib_disable(struct btrfs_fs_info *fs_info);
int btrfs_wib_request_enable(struct btrfs_fs_info *fs_info, bool automatic);

int btrfs_wib_mark(struct btrfs_fs_info *fs_info, u64 logical, u64 len);
void btrfs_wib_note_written(struct btrfs_fs_info *fs_info, u64 full_stripe_start,
			    int nr_data, u64 cols, u32 par, bool logged);
void btrfs_wib_note_written_data(struct btrfs_fs_info *fs_info, u64 logical, u64 len);
void btrfs_wib_done(struct btrfs_fs_info *fs_info, u64 logical, u64 len, bool failed);
void btrfs_wib_mark_stale(struct btrfs_fs_info *fs_info, u64 logical, u64 len);
bool btrfs_wib_stale(struct btrfs_fs_info *fs_info, u64 logical);
void btrfs_wib_clear_stale(struct btrfs_fs_info *fs_info, u64 logical, u64 len,
			   bool durable);
bool btrfs_wib_any_stale(const struct btrfs_fs_info *fs_info);
bool btrfs_wib_persisting(struct btrfs_fs_info *fs_info);
bool btrfs_wib_replace_mark_stale(struct btrfs_fs_info *fs_info, u64 logical,
				  bool owned);
bool btrfs_wib_replace_mark_parity(struct btrfs_fs_info *fs_info,
				   u64 full_stripe_start, int parity, bool owned);
int btrfs_wib_replace_end(struct btrfs_fs_info *fs_info, bool finished);
bool btrfs_wib_replace_marks_lost(struct btrfs_fs_info *fs_info);
bool btrfs_wib_replace_resume_rewinds(struct btrfs_fs_info *fs_info, u64 nr_uncopyable);

/*
 * What the log knows about one full stripe, as the read and scrub paths need
 * it: which data columns cannot be believed, and which parities cannot be
 * used to replace them.
 */
struct btrfs_wib_stripe_state {
	/* Bit i: data column i of the full stripe is recorded stale. */
	u64 stale_cols;
	/* Bit p: parity p of the full stripe is recorded stale. */
	u32 bad_parity;
	/*
	 * Newest generation at which any region covering this stripe gained a
	 * fault record.  A logical address is reused once its extent is freed,
	 * so without this a helper cannot tell whether the extent it finds
	 * there now is the one that was damaged.
	 */
	u64 gen;
};

enum btrfs_wib_stripe_error {
	BTRFS_WIB_STRIPE_NO_ERROR,
	/* Some data blocks of the full stripe carry an error record, not all. */
	BTRFS_WIB_STRIPE_PARTIAL_ERROR,
	BTRFS_WIB_STRIPE_ERROR,
};

enum btrfs_wib_stripe_error btrfs_wib_stripe_error(struct btrfs_fs_info *fs_info,
						   u64 full_stripe_start, int nr_data);
bool btrfs_wib_stripe_state(struct btrfs_fs_info *fs_info, u64 full_stripe_start,
			    int nr_data, int nr_parity,
			    struct btrfs_wib_stripe_state *st);
void btrfs_wib_update_stale_parity(struct btrfs_fs_info *fs_info,
				   u64 full_stripe_start, int parity, bool stale);
void btrfs_wib_mark_suspect_parity(struct btrfs_fs_info *fs_info,
				   u64 full_stripe_start, u64 len, int parity);
void btrfs_wib_keep_prior_verdict(struct btrfs_fs_info *fs_info,
				  u64 full_stripe_start, u32 parities);
/*
 * A copy of the data columns of one full stripe a scrub declined to repair,
 * taken at the verdict from the buffers the scrub already holds.
 */
#define BTRFS_RAID56_EVIDENCE_SLOTS	4
/* 16 columns of BTRFS_STRIPE_LEN.  Wider stripes are named but not copied. */
#define BTRFS_RAID56_EVIDENCE_MAX_BYTES	(16 * BTRFS_STRIPE_LEN)

struct btrfs_raid56_evidence_slot {
	u64 full_stripe_start;
	u64 gen;
	u64 stale_cols;
	u64 bad_parity;
	u64 record_flags;
	u32 nr_data;
	u32 nr_parity;
	u32 nr_bytes;
	u64 devid[BTRFS_RAID56_EVIDENCE_MAX_COLS];
	u64 physical[BTRFS_RAID56_EVIDENCE_MAX_COLS];
	void *data;
};

/* Every field is protected by fs_info->raid56_evidence_lock. */
struct btrfs_raid56_evidence {
	/*
	 * The file ARM_BIND tied the channel to, or NULL.  Only ever compared
	 * against a file being released, never dereferenced.  Holding a
	 * reference instead would keep open the very file whose closing is
	 * supposed to disarm the channel.
	 */
	const struct file *owner;
	u32 head;
	u32 nr;
	/*
	 * A capture has already waited out the full grace period for a slot
	 * once and nothing drained.  Set so the rest of the scrub does not pay
	 * that wait per declined stripe; cleared as soon as a read arrives,
	 * because a helper that is reading again is a helper worth waiting for.
	 */
	bool stalled;
	u64 dropped_full;
	u64 dropped_wide;
	u64 waited;
	u64 captured;
	struct btrfs_raid56_evidence_slot slots[BTRFS_RAID56_EVIDENCE_SLOTS];
};

int btrfs_raid56_evidence_arm(struct btrfs_fs_info *fs_info, const struct file *owner);
int btrfs_raid56_evidence_disarm_request(struct btrfs_fs_info *fs_info, bool if_empty);
void btrfs_raid56_evidence_disarm(struct btrfs_fs_info *fs_info);
void btrfs_raid56_evidence_file_released(struct btrfs_fs_info *fs_info,
					 const struct file *file);
bool btrfs_raid56_evidence_armed(const struct btrfs_fs_info *fs_info);
struct btrfs_raid56_evidence_slot *
btrfs_raid56_evidence_claim(struct btrfs_fs_info *fs_info, u32 nr_data, u32 nr_parity);
void btrfs_raid56_evidence_commit(struct btrfs_fs_info *fs_info);
int btrfs_raid56_evidence_take(struct btrfs_fs_info *fs_info,
			       struct btrfs_ioctl_raid56_evidence_args *args,
			       void __user *ubuf);

void btrfs_wib_forget_range(struct btrfs_fs_info *fs_info, u64 logical, u64 len);
void btrfs_wib_commit_prepare(struct btrfs_fs_info *fs_info);
int btrfs_wib_commit(struct btrfs_fs_info *fs_info, bool flushed);

/* Helpers exported for the self tests. */
u64 btrfs_wib_range_mask(u64 bytenr, u64 logical, u64 len);
int btrfs_wib_build_block(struct btrfs_wib *wib, void *block, u64 seq, const void *base);
bool btrfs_wib_block_valid(const struct btrfs_fs_info *fs_info, const void *block);
/* Decode entry @i of an on-disk block, whichever format it is in. */
void btrfs_wib_read_entry(const void *block, u32 i, struct btrfs_wib_entry *out);
bool btrfs_wib_block_marks_torn(const void *block);
int btrfs_wib_load_block(struct btrfs_wib *wib, const void *block, bool newest,
			 unsigned int *nr_torn);
bool btrfs_wib_block_drops(const void *old, const void *new);
int btrfs_wib_add_pending(struct btrfs_wib *wib,
			  const struct btrfs_wib_entry *src);
void btrfs_wib_finalize_pending(struct btrfs_wib *wib);
int btrfs_wib_try_mark(struct btrfs_wib *wib, u64 logical, u64 len);
bool btrfs_wib_can_mark(struct btrfs_wib *wib, u64 logical, u64 len);
bool btrfs_wib_readd_refuses(struct btrfs_wib *wib, u64 logical, u64 len);
void btrfs_wib_add_sticky(struct btrfs_fs_info *fs_info, u64 logical, u64 len);
int btrfs_wib_try_add_sticky(struct btrfs_fs_info *fs_info, u64 logical, u64 len);
void btrfs_wib_clear_sticky(struct btrfs_fs_info *fs_info, u64 logical, u64 len);
int btrfs_wib_snapshot(struct btrfs_fs_info *fs_info, u64 from,
		       struct btrfs_wib_entry *out);

#endif
