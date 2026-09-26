// SPDX-License-Identifier: GPL-2.0
/*
 * RAID56 write-intent log.
 *
 * [THE PROBLEM]
 *
 * A sub-stripe write on a RAID5/6 block group is a read-modify-write of the
 * full stripe: the data sectors of the higher layer bios and the recomputed
 * P/Q sectors are written as independent bios to independent devices.  If
 * the machine crashes after some of those writes reached their device and
 * others did not, the vertical stripes touched by the RMW are inconsistent:
 * P/Q no longer match the data.
 *
 * The newly written data sectors are unreferenced (btrfs is copy-on-write,
 * the transaction referencing them never committed), but the other sectors
 * of the same vertical stripes belong to committed extents.  They are still
 * readable, but they lost their redundancy: if the device holding them
 * fails, rebuilding them from the stale parity yields garbage.  This is the
 * classic RAID5/6 write hole.  In-place writes (nodatacow, preallocated
 * extents) are affected even when they cover the full stripe, because the
 * sectors being overwritten are themselves referenced.
 *
 * [THE FIX]
 *
 * Before the writes of such a RMW are submitted, the full stripe is recorded
 * in a small log that lives at a fixed location on every writable device,
 * and the log write is made durable (FUA).  After all writes of the RMW have
 * completed the stripe is removed from the in-memory set; the on-disk log is
 * rewritten lazily: the log writes issued for new RMWs only ever add to the
 * on-disk block, and a stripe is dropped from it only by the transaction
 * (or log) commit, after the commit's device barriers confirmed that every
 * device flushed its cache, so that the stripe's data and parity writes are
 * on stable media before the log stops mentioning the stripe.
 *
 * On mount, the newest valid log block of every present device is read,
 * the union taken, and every listed full stripe is scrubbed
 * (btrfs_scrub_raid56_full_stripe()): every sector holding an extent is
 * verified by checksum, bad ones are rebuilt from the existing parity, and
 * P/Q are regenerated.  Only afterwards is the filesystem written to.
 *
 * [RECORD KINDS]
 *
 * A record is either clean (the stripe had a write in flight on a fully
 * working array) or an error record (a write to the stripe failed on some
 * device, the record of a dropped stripe may not have reached a device with
 * its data flushed, or a previous recovery could not finish).
 *
 * For a clean record every present device holds what was last written to
 * it, so after the extents visible in the commit roots have been verified,
 * the parity of every vertical stripe is recomputed from the data as it is
 * on disk.  This also covers extents that are only referenced from the tree
 * log (fsync'ed data and the log tree blocks themselves), which the extent
 * tree does not know yet at that point.
 *
 * For an error record a device may hold stale sectors, so only sectors that
 * can be verified are trusted: the parity is recomputed from verified data
 * only, and if a sector cannot be repaired the parity is left alone.  The
 * record stays until a later pass can verify everything: after the tree log
 * has been replayed (so that its extents are visible), or at a later mount
 * once a missing device is back or replaced.
 *
 * [INVARIANT]
 *
 * At any instant, every full stripe that has a write whose data or parity
 * may not yet be on stable media is listed in the newest valid on-disk log
 * block of enough devices to survive the RAID's tolerated number of device
 * failures.  Proof sketch:
 *
 *  - A RMW proceeds to its writes only after a commit whose snapshot
 *    contained its stripe completed on enough devices (btrfs_wib_mark()).
 *  - A stripe is dropped from a snapshot only after btrfs_wib_done(), i.e.
 *    after all its writes completed at the device level, and only after a
 *    flush of every device that every device confirmed, which pushes those
 *    completed writes to stable media before the new log block lands.  If
 *    a device did not confirm the flush, the stripes are kept in the block
 *    as error records instead -- the ones about to be dropped, and the ones
 *    with a write in flight, whose earlier writes that device may have lost
 *    just the same -- naming its column or parity wherever a write went to
 *    it since it last confirmed a flush and a block can describe the names
 *    (it may hold stale sectors there).  The in-memory set
 *    takes them all back before any later drop, since a later flush does
 *    not bring back what a failed one lost; only when no block could ever
 *    describe them all does it keep what fits, and says so
 *    (wib_readd_dropped()).  All other blocks are supersets of the
 *    previous one, so a device that misses a write keeps a block that
 *    lists everything it may hold unflushed.
 *  - Each device alternates between its two slots and only advances after
 *    a successful write, so the block being overwritten on a device is
 *    never its newest valid one; a torn write invalidates at most the newest
 *    block, and the previous block is a superset with respect to the
 *    stripes that could still be in flight (see the points above).
 *  - Recovery uses the union of all devices' newest valid blocks.  Stale
 *    blocks (from an old device that re-joined, or a device that missed
 *    commits) can only add stripes to scrub, which is harmless: scrubbing
 *    a consistent stripe is a no-op.
 *
 * [LIMITS]
 *
 * If the array is degraded (a device missing) when the crash happens, a
 * vertical stripe whose missing sector was committed data and whose parity
 * was in the middle of an update cannot be reconstructed with certainty.
 * Recovery then relies on data checksums: the rebuild is verified and
 * refused when it does not match, so the loss is detected, not silent.  Data
 * without a checksum has nothing to verify against, so recovery classifies
 * such a stripe instead of rebuilding it (scrub_raid56_recover_absent()),
 * whether or not its record names the missing column: on RAID6 it compares
 * the column's rebuild from P with its rebuild from Q, and what it cannot
 * decide it records as undecidable, every present parity stale, so that a
 * read fails rather than returns a guess (a verdict the log writes as the
 * stripe's torn mark, and every later mount reaches again, or keeps from a
 * wide block: @suspect_par and @prior_par in struct btrfs_wib_entry).  A
 * read-only mount runs no recovery; until one does, a rebuild of such data
 * that no parity is left over to cross-check is refused wherever the log
 * recorded a write into the stripe that may have been torn
 * (btrfs_wib_unrecovered()).  Getting such data back,
 * rather than refusing it, takes the device coming back, or journaling the
 * data itself, which this log does not do.
 *
 * "May have been torn" is a write in flight at the crash, or one a failed
 * flush may have taken part of without the record being able to say which
 * member (@torn in struct btrfs_wib_entry, written as in flight).  A record
 * of a plain failed write names the member it left stale, and nothing else is
 * in doubt: its stripe is rebuilt as any other.  Telling the two apart takes
 * a writer that marks the first kind, which a block says it is
 * (BTRFS_WIB_TORN_MARKING); every error record of a block that does not say
 * so is taken for the first kind.
 */

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/bsearch.h>
#include <linux/mm.h>
#include <linux/rcupdate.h>
#include <linux/sched/mm.h>
#include <linux/delay.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "messages.h"
#include "ctree.h"
#include "fs.h"
#include "volumes.h"
#include "raid56.h"
#include "raid56-wib.h"
#include "scrub.h"
#include "disk-io.h"
#include "accessors.h"
#include "zoned.h"

static void raid56_alert_kick(struct btrfs_wib *wib, unsigned long delay);
static void raid56_alert_failed(struct btrfs_fs_info *fs_info, enum btrfs_raid56_event ev,
				u64 logical, const struct btrfs_wib_flush_failed *failed);

/*
 * Do not consult the record read off the disk until a read-write recovery
 * takes it over, as before it was consulted from the first read at mount.
 * The negative control for tools/testing/btrfs/uml/early_record.sh.
 */
static bool btrfs_raid56_no_early_record;
#ifdef CONFIG_BTRFS_DEBUG
module_param_named(raid56_no_early_record, btrfs_raid56_no_early_record, bool, 0644);
MODULE_PARM_DESC(raid56_no_early_record,
		 "Ignore the write-intent record read at mount until recovery runs (testing only: restores the old behaviour)");
#endif

#ifdef CONFIG_BTRFS_DEBUG
/*
 * Write the stale record as zero, i.e. behave exactly as the format did
 * before it carried one.  The negative control for the cross-mount
 * reproduction: same kernel, one flag, so a pass with the record persisted
 * means something.  See tools/testing/btrfs/uml/nocow_persist.sh.
 */
static bool stale_no_persist;
module_param_named(raid56_stale_no_persist, stale_no_persist, bool, 0644);
MODULE_PARM_DESC(raid56_stale_no_persist,
		 "Do not persist the stale record, as the format did before it had one (testing only: restores a known defect)");
#define wib_no_persist()	READ_ONCE(stale_no_persist)
#else
#define wib_no_persist()	false
#endif

/*
 * Testing only.  raid56_wf_all_records_torn=1: treat every record read at
 * mount as possibly torn -- the rules for a stripe with a write in flight
 * (scrub_raid56_recover_absent(), btrfs_wib_unrecovered()) apply to records of
 * plain failed writes too -- and mark nothing possibly torn (@torn in struct
 * btrfs_wib_entry), as before the mark existed.  Safe, and it refuses reads
 * that were fine: uml/early_record.sh's all-records arms.
 *
 * raid56_wf_torn_no_persist=1: keep the mark, in memory only.  The negative
 * control for uml/torn_readd.sh: a readd that turned a possibly torn write
 * into a plain record then reads, after a crash, like a plain failed write.
 *
 * raid56_wf_log_unmarked=1: write blocks without BTRFS_WIB_TORN_MARKING, as a
 * kernel from before the mark did.  With raid56_wf_no_readd_name=1 as well,
 * the log a failed flush leaves is the one such a kernel left: the upgrade
 * arms of uml/torn_readd.sh.
 *
 * raid56_wf_trust_unmarked_log=1: read the error records of a block without
 * BTRFS_WIB_TORN_MARKING as plain failed writes, as before the marker
 * existed.  The negative control for those arms.
 *
 * raid56_wf_suspect_as_stale=1: treat the mount's verdict on a possibly torn
 * stripe it cannot decide (@suspect_par in struct btrfs_wib_entry) as any
 * stale parity: it makes the log block wide, halving what the log can hold,
 * and a full log with a device missing spends it in table order
 * (wib_evict_sticky()) -- so that a degraded mount keeping more such stripes
 * than a wide block holds spends some of them, and their reads return a
 * rebuild from the torn parity.  The negative control for the upgrade-verdict
 * arm of uml/torn_readd.sh.
 *
 * raid56_wf_replace_end_clears_verdicts=1: let the end of a device replace
 * turn every verdict (@suspect_par) into an ordinary stale parity as it
 * releases its own marks, and leave the live set as big as it was: a log
 * holding more verdicts than a wide block describes can no longer be written,
 * and a full log with a device missing spends them in table order.  The
 * negative control for the replace arm of uml/verdict_keep.sh.
 *
 * raid56_wf_reload_verdicts_plain=1: take the verdict an earlier mount wrote
 * in a wide block, which reloads as a stale parity, for any record naming a
 * member (no @prior_par): a full log with a device missing spends it in table
 * order.  The negative control for the remount arm of uml/verdict_keep.sh.
 *
 * raid56_wf_recover_drops_refusals=1: let the recovery stop answering from
 * the records read at mount before it has recovered a single stripe, and not
 * answer from them again when it stops early: a read-only mount made
 * read-write stops refusing unchecked rebuilds in stripes the recovery has not
 * reached (btrfs_wib_unrecovered()), and stops reading the columns their
 * records name stale as stale -- for good when the remount fails.  The
 * negative control for uml/recover_interrupt.sh.
 *
 * raid56_wf_missing_parity_keeps_torn=1: keep a stripe the recovery found
 * consistent but for a parity on a missing device marked possibly torn, as
 * before btrfs_wib_parity_unwritten(), rather than recording that parity
 * stale: a RAID6 read that has to rebuild a sector from the parity the
 * recovery just regenerated fails as read_unrecovered.  The negative control
 * for the misspar arm of uml/torn_present.sh.
 *
 * raid56_wf_absent_decided_keeps_torn=1: keep a RAID6 stripe marked possibly
 * torn, as before BTRFS_WIB_STRIPE_DECIDED, when the recovery decided its data
 * column on a missing device -- both parities agree about it -- and
 * regenerated both parities from the data (scrub_raid56_absent_pq()): a read
 * that loses a second column there, and has to rebuild both from the parities
 * that now describe them, fails as read_unrecovered.  The negative control
 * for the decided arm of uml/torn_present.sh.
 *
 * raid56_wf_recovering_stripe_readable=1: while the recovery decides a full
 * stripe a write may have torn, let reads of it that are not the recovery's
 * own go unrefused, as before btrfs_wib_recovering(): it has taken the
 * stripe over, so @pending no longer answers for it, and its verdict is not
 * in yet.  A rebuild with no parity left over to check it is returned as
 * data.  The negative control for the selfctl arm of
 * uml/recover_interrupt.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool all_records_torn;
module_param_named(raid56_wf_all_records_torn, all_records_torn, bool, 0644);
MODULE_PARM_DESC(raid56_wf_all_records_torn,
		 "Treat every write-intent record read at mount as possibly torn, and mark none (testing only: restores the old behaviour)");
static bool torn_no_persist;
module_param_named(raid56_wf_torn_no_persist, torn_no_persist, bool, 0644);
MODULE_PARM_DESC(raid56_wf_torn_no_persist,
		 "Do not write the possibly-torn mark to the write-intent log (testing only: restores a known defect)");
static bool log_unmarked;
module_param_named(raid56_wf_log_unmarked, log_unmarked, bool, 0644);
MODULE_PARM_DESC(raid56_wf_log_unmarked,
		 "Write write-intent log blocks without the marker saying possibly torn records are marked, as older kernels did (testing only: restores the old behaviour)");
static bool trust_unmarked_log;
module_param_named(raid56_wf_trust_unmarked_log, trust_unmarked_log, bool, 0644);
MODULE_PARM_DESC(raid56_wf_trust_unmarked_log,
		 "Read the error records of a write-intent log block written by a kernel that does not mark possibly torn records as plain failed writes (testing only: restores a known defect)");
static bool suspect_as_stale;
module_param_named(raid56_wf_suspect_as_stale, suspect_as_stale, bool, 0644);
MODULE_PARM_DESC(raid56_wf_suspect_as_stale,
		 "Treat the recovery's verdict on a possibly torn stripe it cannot decide as any stale parity: it halves the write-intent log, and a degraded full log spends it in table order (testing only: restores a known defect)");
static bool replace_end_clears_verdicts;
module_param_named(raid56_wf_replace_end_clears_verdicts, replace_end_clears_verdicts,
		   bool, 0644);
MODULE_PARM_DESC(raid56_wf_replace_end_clears_verdicts,
		 "Let the end of a device replace turn the recovery's verdicts on torn stripes into ordinary stale parities, without fitting the write-intent log to the wide layout that takes (testing only: restores a known defect)");
static bool reload_verdicts_plain;
module_param_named(raid56_wf_reload_verdicts_plain, reload_verdicts_plain, bool, 0644);
MODULE_PARM_DESC(raid56_wf_reload_verdicts_plain,
		 "Let a degraded full write-intent log spend an earlier mount's verdict on a torn stripe, reloaded from a wide log block, in table order (testing only: restores a known defect)");
static bool recover_drops_refusals;
module_param_named(raid56_wf_recover_drops_refusals, recover_drops_refusals, bool, 0644);
MODULE_PARM_DESC(raid56_wf_recover_drops_refusals,
		 "Let the write-intent log recovery drop the read-only mount's refusals for every stripe it lists before recovering any, and for good when it stops early (testing only: restores a known defect)");
static bool missing_parity_keeps_torn;
module_param_named(raid56_wf_missing_parity_keeps_torn, missing_parity_keeps_torn, bool, 0644);
MODULE_PARM_DESC(raid56_wf_missing_parity_keeps_torn,
		 "Keep a stripe the write-intent log recovery found consistent but for a parity on a missing device marked possibly torn, refusing rebuilds from the parity it regenerated (testing only: restores a known defect)");
static bool absent_decided_keeps_torn;
module_param_named(raid56_wf_absent_decided_keeps_torn, absent_decided_keeps_torn, bool, 0644);
MODULE_PARM_DESC(raid56_wf_absent_decided_keeps_torn,
		 "Keep a RAID6 stripe marked possibly torn after the write-intent log recovery decided its data column on a missing device and regenerated both parities, refusing two-column rebuilds from them (testing only: restores a known defect)");
static bool recovering_stripe_readable;
module_param_named(raid56_wf_recovering_stripe_readable, recovering_stripe_readable, bool,
		   0644);
MODULE_PARM_DESC(raid56_wf_recovering_stripe_readable,
		 "While the write-intent log recovery decides a full stripe a write may have torn, return rebuilds of it nothing checked to readers other than the recovery (testing only: restores a known defect)");
#else
static const bool all_records_torn;
static const bool torn_no_persist;
static const bool log_unmarked;
static const bool trust_unmarked_log;
static const bool suspect_as_stale;
static const bool replace_end_clears_verdicts;
static const bool reload_verdicts_plain;
static const bool recover_drops_refusals;
static const bool missing_parity_keeps_torn;
static const bool absent_decided_keeps_torn;
static const bool recovering_stripe_readable;
#endif

#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
/* The self tests run at load, after the command line has set the knobs. */
bool btrfs_wib_all_records_torn(void)
{
	return READ_ONCE(all_records_torn);
}

bool btrfs_wib_torn_no_persist(void)
{
	return READ_ONCE(torn_no_persist);
}

bool btrfs_wib_log_unmarked(void)
{
	return READ_ONCE(log_unmarked);
}

bool btrfs_wib_trust_unmarked_log(void)
{
	return READ_ONCE(trust_unmarked_log);
}

bool btrfs_wib_missing_parity_keeps_torn(void)
{
	return READ_ONCE(missing_parity_keeps_torn);
}

bool btrfs_wib_absent_decided_keeps_torn(void)
{
	return READ_ONCE(absent_decided_keeps_torn);
}

bool btrfs_wib_suspect_as_stale(void)
{
	return READ_ONCE(suspect_as_stale);
}

bool btrfs_wib_replace_end_clears_verdicts(void)
{
	return READ_ONCE(replace_end_clears_verdicts);
}

bool btrfs_wib_reload_verdicts_plain(void)
{
	return READ_ONCE(reload_verdicts_plain);
}
#endif

static_assert(sizeof(struct btrfs_wib_disk_header) == 128);
static_assert(sizeof(struct btrfs_wib_disk_entry_v1) == 24);
static_assert(BTRFS_WIB_MAX_ENTRIES_V1 == 165);
static_assert(sizeof(struct btrfs_wib_disk_entry) == 48);
static_assert(BTRFS_WIB_MAX_ENTRIES == 82);
/* Neither layout's entries reach the marker. */
static_assert(sizeof(struct btrfs_wib_disk_header) +
	      BTRFS_WIB_MAX_ENTRIES_V1 * sizeof(struct btrfs_wib_disk_entry_v1) <=
	      BTRFS_WIB_TRAILER_OFFSET);
static_assert(sizeof(struct btrfs_wib_disk_header) +
	      BTRFS_WIB_MAX_ENTRIES * sizeof(struct btrfs_wib_disk_entry) <=
	      BTRFS_WIB_TRAILER_OFFSET);
/*
 * @stale is a strict subset of @error, and btrfs_wib_block_valid() enforces
 * that on read.  The two parity bits of a full stripe live in @stale_par at
 * the block of the stripe's start and the one after it, which is why nr_data
 * being at least 2 for every RAID5/6 chunk matters.
 */
static_assert(BTRFS_WIB_BLOCK_SHIFT == BTRFS_STRIPE_LEN_SHIFT);
static_assert(BTRFS_WIB_OFFSET + BTRFS_WIB_NR_SLOTS * BTRFS_WIB_SLOT_SIZE <=
	      BTRFS_DEVICE_RANGE_RESERVED);
static_assert(BTRFS_SUPER_INFO_OFFSET + BTRFS_SUPER_INFO_SIZE <= BTRFS_WIB_OFFSET);
static_assert(BTRFS_WIB_BLOCK_SHIFT == BTRFS_STRIPE_LEN_SHIFT);

static inline u64 wib_entry_bytenr(u64 logical)
{
	return logical & ~(BTRFS_WIB_ENTRY_SIZE - 1);
}

/*
 * Return the bitmap of blocks of the entry at @bytenr that intersect
 * [@logical, @logical + @len).
 */
u64 btrfs_wib_range_mask(u64 bytenr, u64 logical, u64 len)
{
	const u64 start = max(logical, bytenr);
	const u64 end = min(logical + len, bytenr + BTRFS_WIB_ENTRY_SIZE);
	unsigned int first;
	unsigned int last;

	if (start >= end)
		return 0;
	first = (start - bytenr) >> BTRFS_WIB_BLOCK_SHIFT;
	last = (end - 1 - bytenr) >> BTRFS_WIB_BLOCK_SHIFT;
	return GENMASK_ULL(last, first);
}

static bool wib_entry_used(const struct btrfs_wib_entry *e)
{
	return (e->bitmap | e->sticky) != 0;
}

/*
 * Set @e->stale_par, keeping wib->nr_stale in step.  Caller holds wib->lock.
 *
 * nr_stale gates every staleness query on a lock-free "nothing is recorded"
 * fast path, and it used to count only stale DATA.  A stripe whose only
 * record was a parity that did not describe the data was then invisible to
 * all of them: a degraded read rebuilt a missing column out of that parity,
 * and for a sector with no checksum returned the result as the file's
 * content.  Counting the parity marks too costs only the slow path while one
 * exists, which is what that fast path was always meant to trade.
 */
static void wib_set_stale_par(struct btrfs_wib *wib, struct btrfs_wib_entry *e,
			      u64 val)
{
	const int delta = hweight64(val) - hweight64(e->stale_par);

	lockdep_assert_held(&wib->lock);
	if (delta)
		atomic_add(delta, &wib->nr_stale);
	e->stale_par = val;
	e->suspect_par &= val;
	e->prior_par &= val;
	e->replace_keep_par &= val;
}

/*
 * The @stale and @stale_par bits of @e are being set or cleared by something
 * other than a running device replace: whatever the replace had marked there
 * is no longer its own to hide or to drop (see @replace_stale in struct
 * btrfs_wib_entry).  Every writer of @stale or @stale_par calls this for the
 * bits it touches, so that an abort never takes back a mark that also says
 * something about the source.  Nor is a parity mark then only the recovery's
 * verdict, which the torn mark would reproduce (@suspect_par): it is written
 * as what it is.  Caller holds wib->lock.
 */
static void wib_replace_disown(struct btrfs_wib_entry *e, u64 stale, u64 stale_par)
{
	e->replace_stale &= ~stale;
	e->replace_stale_par &= ~stale_par;
	e->suspect_par &= ~stale_par;
	e->prior_par &= ~stale_par;
}

static struct btrfs_wib_entry *wib_find_entry(struct btrfs_wib *wib, u64 bytenr)
{
	lockdep_assert_held(&wib->lock);

	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		struct btrfs_wib_entry *e = &wib->entries[i];

		if (wib_entry_used(e) && e->bytenr == bytenr)
			return e;
	}
	return NULL;
}

/*
 * Does @e need the wide layout?  A stale record does -- except a parity mark
 * that is only the recovery's verdict on a stripe the entry marks possibly
 * torn (@suspect_par in struct btrfs_wib_entry): the narrow layout writes the
 * torn mark, and the next mount reaches the verdict again from it.
 *
 * Counted as stale, the verdict halved the log for everything in it.  A
 * degraded mount's recovery keeping more such stripes than a wide block holds
 * -- say a log from before the mark, where every error record is possibly
 * torn (btrfs_wib_load_block()) -- then had to spend some to keep the others,
 * and a full log spends a record naming a member while a device is missing
 * (wib_may_evict_naming()): the verdict went with it, and a read of the
 * column's unchecksummed data was rebuilt from the torn parity and returned.
 * Not without the torn mark to fall back on: raid56_wf_torn_no_persist=1, or
 * raid56_wf_all_records_torn=1, under which nothing is marked.
 */
static bool wib_entry_wide(const struct btrfs_wib_entry *e)
{
	u64 verdict = e->suspect_par & e->torn;

	if (READ_ONCE(suspect_as_stale) || READ_ONCE(torn_no_persist))
		verdict = 0;
	return (e->stale | (e->stale_par & ~verdict)) != 0;
}

/*
 * Does @e carry the recovery's verdict on a stripe (@suspect_par), or what
 * may be an earlier mount's (@prior_par)?
 */
static bool wib_entry_verdict(const struct btrfs_wib_entry *e)
{
	return (e->suspect_par && !READ_ONCE(suspect_as_stale)) || e->prior_par;
}

/*
 * How many entries the log may hold right now.
 *
 * A block carrying a stale record must be written in the wide layout, whose
 * entry is twice the size, so the same 4KiB slot expresses half as many
 * regions.  The in-memory table is sized to the NARROW maximum so that a
 * filesystem that has never had a stale record can use all of it -- which
 * means the table can hold more than a wide block is able to describe.
 *
 * A set that cannot be described cannot be written.  btrfs_wib_build_block()
 * returns -ENOSPC and leaves the block zeroed with no magic, and its callers
 * are not all in a position to notice: wib_flush_and_drop_locked() asserts
 * that it fits and then hands the block on as the snapshot of what was in
 * flight, so on a kernel built without CONFIG_BTRFS_ASSERT an empty snapshot
 * is taken to mean nothing was in flight, and stripes that finished after the
 * flush are dropped from the log without a flush having covered them.  That is
 * the write hole this log exists to close, reopened by a capacity accident.
 *
 * So the live set is capped here instead, and the assertion is allowed to be
 * true.
 */
static u32 wib_live_max(const struct btrfs_wib *wib)
{
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		const struct btrfs_wib_entry *e = &wib->entries[i];

		if (wib_entry_used(e) && wib_entry_wide(e))
			return BTRFS_WIB_MAX_ENTRIES;
	}
	return BTRFS_WIB_MAX_ENTRIES_V1;
}

static u32 wib_live_count(const struct btrfs_wib *wib)
{
	u32 nr = 0;

	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++)
		if (wib_entry_used(&wib->entries[i]))
			nr++;
	return nr;
}

/*
 * True if this entry names the member a failed write left wrong -- or says a
 * write into it may have been torn (@torn), which no scrub can find out again
 * by reading either.
 */
static bool wib_entry_names_member(const struct btrfs_wib_entry *e)
{
	return (e->stale | e->stale_par | e->torn) != 0;
}

/*
 * Make room by dropping an entry that only records stripes without a write
 * in flight (sticky).  Those stripes then rely on scrub instead of the
 * mount-time recovery.
 *
 * Two passes, because the records are not worth the same.  An entry carrying
 * @stale or @stale_par names WHICH member a failed write left wrong, and for
 * data with no checksum that is the only thing in the system that can tell a
 * stale data column from a stale parity -- the two need opposite repairs, and
 * a scrub that cannot tell them apart may not touch the stripe at all.  An
 * entry with only @sticky says that something went wrong somewhere in the
 * stripe, which a scrub can rediscover by reading it.  So only the vague
 * records are ever spent -- and, when nothing else is left, those that only
 * say a write may have been torn (wib_entry_torn_only()).
 *
 * A log full of specific ones fails the new write instead (btrfs_wib_mark()):
 * the application sees EIO and the user an alert, where dropping the record
 * would have lost acknowledged data with no sign at all.  The records retire
 * as repairs reach their stripes.  wib_count_entries_locked() counts only
 * what wib_evictable() allows as room, so btrfs_wib_try_mark() is never
 * promised room this cannot find.
 */
/*
 * Testing only: let a full log evict records that name a stale member again.
 * See wib_evict_sticky().
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool evict_naming;
module_param_named(raid56_evict_naming, evict_naming, bool, 0644);
MODULE_PARM_DESC(raid56_evict_naming,
		 "Let a full write-intent log evict records that name a stale member (testing only: restores a known defect)");
static bool keep_naming_degraded;
module_param_named(raid56_keep_naming_degraded, keep_naming_degraded, bool, 0644);
MODULE_PARM_DESC(raid56_keep_naming_degraded,
		 "Never let a full write-intent log spend records naming a stale member, even with a device missing (testing only: restores a known wedge)");
/*
 * raid56_wf_evict_replace_marks=1: a full log with a device missing spends a
 * running device replace's own marks in table order, like any record naming a
 * member, and the replace finishes without them.  The negative control for
 * uml/replace_marks_kept.sh.
 */
static bool evict_replace_marks;
module_param_named(raid56_wf_evict_replace_marks, evict_replace_marks, bool, 0644);
MODULE_PARM_DESC(raid56_wf_evict_replace_marks,
		 "Let a full write-intent log with a device missing spend a running device replace's record of the zeros on its new device like any other, and let the replace finish without it (testing only: restores a known defect)");
/*
 * raid56_wf_replace_keeps_added_only=1: a running device replace's record of
 * the zeros it put on its target is its own only where it made the column (or
 * parity) stale itself (@replace_stale), not where it was stale already
 * (@replace_keep in struct btrfs_wib_entry) -- so a full log with a device
 * missing spends it in table order, and the replace finishes without it, where
 * a degraded write had named the column before -- and a replace resumed after
 * a crash or unmount goes on from where it was, leaving the marks it made
 * before, which reload as ordinary ones, to the same fate.  The negative
 * control for the stale and resume arms of uml/replace_marks_kept.sh.
 */
static bool replace_keeps_added_only;
module_param_named(raid56_wf_replace_keeps_added_only, replace_keeps_added_only, bool, 0644);
MODULE_PARM_DESC(raid56_wf_replace_keeps_added_only,
		 "Keep a device replace's record of the zeros on its new device only where it made the column stale itself, and resume an interrupted replace from where it was, so a degraded full write-intent log spends the rest in table order (testing only: restores a known defect)");
/*
 * raid56_wf_torn_unevictable=1: a full log with every device there never
 * spends a record that only says a write may have been torn (@torn in struct
 * btrfs_wib_entry), as before wib_entry_torn_only() -- a failed flush that
 * leaves the log full of them fails every write into a new region until a
 * scrub.  The negative control for the torn arm of uml/flush_wedge.sh.
 */
static bool torn_unevictable;
module_param_named(raid56_wf_torn_unevictable, torn_unevictable, bool, 0644);
MODULE_PARM_DESC(raid56_wf_torn_unevictable,
		 "Never let a full write-intent log spend a record that only says a write may have been torn while every device is present (testing only: restores a known wedge)");
/*
 * raid56_wf_torn_spent_eagerly=1: a full log with every device there counts
 * the records that only say a write may have been torn as room like any other
 * (wib_count_entries_locked()), and a write that finds it full spends one at
 * once -- with a write in flight that would free a slot in milliseconds too.
 * The negative control for the busy arm of uml/flush_wedge.sh.
 *
 * raid56_wf_kept_torn_in_order=1: a full log with every device there spends
 * the records the mount's recovery kept possibly torn (@kept_torn in struct
 * btrfs_wib_entry) in table order with those a failed flush left -- first,
 * since the recovery adds them first.  The negative control for the evict
 * arm of uml/torn_present.sh.
 */
static bool torn_spent_eagerly;
module_param_named(raid56_wf_torn_spent_eagerly, torn_spent_eagerly, bool, 0644);
MODULE_PARM_DESC(raid56_wf_torn_spent_eagerly,
		 "Let a full write-intent log spend a record that only says a write may have been torn while a write in flight can still make room (testing only: restores a known defect)");
static bool kept_torn_in_order;
module_param_named(raid56_wf_kept_torn_in_order, kept_torn_in_order, bool, 0644);
MODULE_PARM_DESC(raid56_wf_kept_torn_in_order,
		 "Let a full write-intent log spend the records the mount's recovery kept possibly torn in table order with those a failed flush left (testing only: restores a known defect)");
#else
static const bool evict_naming;
static const bool keep_naming_degraded;
static const bool evict_replace_marks;
static const bool replace_keeps_added_only;
static const bool torn_unevictable;
static const bool torn_spent_eagerly;
static const bool kept_torn_in_order;
#endif

#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
/* The self tests run at load, after the command line has set the knob. */
bool btrfs_wib_evicts_naming(void)
{
	return READ_ONCE(evict_naming);
}

bool btrfs_wib_keeps_naming_degraded(void)
{
	return READ_ONCE(keep_naming_degraded);
}

bool btrfs_wib_evicts_replace_marks(void)
{
	return READ_ONCE(evict_replace_marks);
}

bool btrfs_wib_replace_keeps_added_only(void)
{
	return READ_ONCE(replace_keeps_added_only);
}

bool btrfs_wib_torn_unevictable(void)
{
	return READ_ONCE(torn_unevictable);
}

bool btrfs_wib_torn_spent_eagerly(void)
{
	return READ_ONCE(torn_spent_eagerly);
}

bool btrfs_wib_kept_torn_in_order(void)
{
	return READ_ONCE(kept_torn_in_order);
}
#endif

/*
 * May a full log spend a record that names a stale member?  Not while that
 * member can still be repaired: such a record retires by itself once the
 * device takes the repair or is replaced, and spending it loses acknowledged
 * data without checksums.
 *
 * But with a device missing, the records naming its columns cannot retire --
 * nothing can be written to it -- and every write of a degraded mount into a
 * stripe with a column there makes another one.  Kept, they wedge the log
 * within a few hundred megabytes: every small write fails, a metadata write
 * turns the filesystem read-only, and 'btrfs replace', which needs it
 * writable, can no longer run.  A degraded array has lost that redundancy
 * anyway, and the returning device is a resync's job.  So spend them then,
 * last, with the alert.  The same during log replay at mount, which must be
 * able to write to finish mounting at all.
 */
static bool wib_may_evict_naming(const struct btrfs_wib *wib)
{
	const struct btrfs_fs_info *fs_info = wib->fs_info;

	if (READ_ONCE(evict_naming))
		return true;
	if (READ_ONCE(keep_naming_degraded))
		return false;
	return READ_ONCE(fs_info->fs_devices->missing_devices) ||
	       test_bit(BTRFS_FS_LOG_RECOVERING, &fs_info->flags);
}

static void wib_policy_locked(struct btrfs_wib *wib)
{
	lockdep_assert_held(&wib->lock);
	wib->may_evict_naming = wib_may_evict_naming(wib);
}

/*
 * Does @e hold, among the bits of @mask, marks a running device replace made
 * for the zeros it put on its target (@replace_stale, @replace_keep in struct
 * btrfs_wib_entry)?
 */
static bool wib_entry_replace_kept(const struct btrfs_wib_entry *e, u64 mask)
{
	const u64 kept = (e->stale & (e->replace_stale | e->replace_keep)) |
			 (e->stale_par & (e->replace_stale_par | e->replace_keep_par));

	return (kept & mask) != 0;
}

static bool wib_entry_replace_owned(const struct btrfs_wib_entry *e)
{
	return wib_entry_replace_kept(e, U64_MAX) && !READ_ONCE(evict_replace_marks);
}

/* The entry at @e is free, or being spent: nothing of a replace's is left. */
static void wib_replace_forget(struct btrfs_wib_entry *e)
{
	wib_replace_disown(e, U64_MAX, U64_MAX);
	e->replace_keep = 0;
	e->replace_keep_par = 0;
}

/*
 * Does @e say no more than that a write into it may have been torn?  Such a
 * record names no member, so nothing repairs it -- a repair rewrites what a
 * record names -- and no write finishing retires it: only a scrub, or a full
 * stripe write, does.  A failed flush that could not name the device leaves
 * a record like that for every stripe it covered (wib_readd_dropped()), up to
 * a log full of them.  So a full log spends them last, when nothing else is
 * left, rather than refuse every write into a new region until a scrub: not
 * while a write in flight can still free a slot (@spend_torn in
 * wib_count_entries_locked(), btrfs_wib_mark()).  What goes is the mark that
 * makes a mount after a crash treat the stripe as the write in flight it was,
 * and that refuses a read of its data without a checksum that would have to
 * be rebuilt from the parity alone: wib_evict_sticky() says so, and raises
 * the alert.
 */
static bool wib_entry_torn_only(const struct btrfs_wib_entry *e)
{
	return e->torn && !(e->stale | e->stale_par) && !READ_ONCE(torn_unevictable);
}

/*
 * Is any of @e's possibly torn mark the recovery's, on a stripe it could not
 * decide (@kept_torn in struct btrfs_wib_entry)?
 */
static bool wib_entry_kept_torn(const struct btrfs_wib_entry *e)
{
	return (e->torn & e->kept_torn) && !READ_ONCE(kept_torn_in_order);
}

/*
 * May a full log evict @e?  See wib_may_evict_naming().  With every device
 * there, a record that only says a write may have been torn only if
 * @spend_torn: see wib_entry_torn_only().
 */
static bool wib_evictable(const struct btrfs_wib *wib, const struct btrfs_wib_entry *e,
			  bool spend_torn)
{
	return !e->bitmap && (wib->may_evict_naming || !wib_entry_names_member(e) ||
			      (spend_torn && wib_entry_torn_only(e)));
}

static struct btrfs_wib_entry *wib_evict_sticky(struct btrfs_wib *wib)
{
	lockdep_assert_held(&wib->lock);

	/*
	 * Pass 0 takes records that only say "a write failed here"; pass 1
	 * used to take records naming the stale member too, and that loses
	 * data: the read path then trusts the stale platter, and the next RMW
	 * or scrub folds it into the parity, silently for data without a
	 * checksum -- the acknowledged value existed only in the parity.  A
	 * full log now fails the new write instead (btrfs_wib_mark()), which
	 * the caller sees -- except when wib_may_evict_naming() says keeping
	 * them would wedge the log.
	 *
	 * And then the recovery's verdicts on stripes it could not decide go
	 * last (pass 3, see wib_entry_verdict()).  Spending one turns a read
	 * that fails into one that returns a rebuild from a torn parity, now:
	 * the verdict is all that stands between them.  The records a
	 * degraded mount makes name the missing device's column, which is
	 * rebuilt anyway, from a parity that describes it.  And verdicts do
	 * not grow -- only the mount's recovery makes them -- so while there
	 * are fewer than a wide block holds, the degraded writes' records
	 * turn over beside them.
	 *
	 * Before the verdicts, a running device replace's own marks (pass 2,
	 * wib_entry_replace_owned()).  They record the zeros it put on its
	 * target where it could neither copy nor rebuild the source, and
	 * once the target takes over nothing else says so: spent in table
	 * order, a replace of the missing device into a full log lost each
	 * such record to the next, finished, and the zeros read back as data.
	 * Spent at all, one now costs the replace instead of data: it fails,
	 * and its target with it (@replace_marks_lost in struct btrfs_wib,
	 * btrfs_wib_replace_end()), and the next transaction commit records
	 * it to resume from the start (btrfs_run_dev_replace()).  A crash
	 * before then can still resume it past the stripe; the alert says to
	 * cancel it in that case.
	 *
	 * With every device there, pass 1 takes only the records that say no
	 * more than that a write may have been torn (wib_entry_torn_only()),
	 * and pass 2 those of them the mount's recovery kept on a stripe it
	 * could not decide (wib_entry_kept_torn()).  Taken in table order they
	 * went first, the recovery having added them first, and one of them
	 * refuses a rebuild from the parity alone where a sector is known not
	 * to read: spent, that read returns the rebuild instead.  Those a
	 * failed flush left refuse one only if a sector fails later.
	 */
	for (int pass = 0; pass < (wib->may_evict_naming ? 4 : 3); pass++) {
		for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
			struct btrfs_wib_entry *e = &wib->entries[i];
			bool lost;

			if (e->bitmap || !e->sticky)
				continue;
			if (pass == 0 && wib_entry_names_member(e))
				continue;
			if (pass >= 1 && !wib->may_evict_naming &&
			    (!wib_entry_torn_only(e) || (pass == 1 && wib_entry_kept_torn(e))))
				continue;
			if (pass == 1 && (wib_entry_verdict(e) || wib_entry_replace_owned(e)))
				continue;
			if (pass == 2 && wib_entry_verdict(e))
				continue;
			lost = wib_entry_replace_owned(e);
			if (!btrfs_is_testing(wib->fs_info)) {
				if (lost)
					btrfs_err_rl(wib->fs_info,
	"raid56 write-intent log full with a device missing, dropping the record of the zeros a running device replace put on its new device at %llu -- the replace will fail rather than let the new device serve them as data",
						     e->bytenr);
				if (e->suspect_par | e->prior_par)
					btrfs_warn_rl(wib->fs_info,
	"raid56 write-intent log full, dropping the record of %u stripes at %llu that could not be decided with a device missing -- their data without a checksum on that device now reads back as a rebuild from a parity a write may have torn, instead of failing",
						      (unsigned int)hweight64(e->sticky),
						      e->bytenr);
				else if (!lost && !(e->stale | e->stale_par) &&
					 (e->torn & e->kept_torn))
					btrfs_warn_rl(wib->fs_info,
	"raid56 write-intent log full, dropping the record of %u stripes at %llu, among them one the mount's recovery could not decide -- its data without a checksum that does not read now comes back as a rebuild from a parity a write may have torn, instead of failing; the files there ('btrfs inspect-internal logical-resolve %llu <mountpoint>') cannot be read back with certainty: restore them from a backup",
						      (unsigned int)hweight64(e->sticky),
						      e->bytenr, e->bytenr);
				else if (!lost && !(e->stale | e->stale_par) && e->torn)
					btrfs_warn_rl(wib->fs_info,
	"raid56 write-intent log full, dropping the record of %u stripes at %llu that a write may have left torn -- their data without a checksum that has to be rebuilt, where a sector does not read or after a crash with a device missing, may now come back as a rebuild from a torn parity instead of failing, run scrub",
						      (unsigned int)hweight64(e->sticky),
						      e->bytenr);
				else if (!lost && wib_entry_names_member(e))
					btrfs_warn_rl(wib->fs_info,
	"raid56 write-intent log full, dropping the record of %u stripes at %llu INCLUDING which member %u of them lost -- that cannot be worked out again by reading the disks, run scrub before those stripes are written",
						      (unsigned int)hweight64(e->sticky),
						      e->bytenr,
						      (unsigned int)hweight64(e->stale | e->stale_par));
				else if (!lost)
					btrfs_warn_rl(wib->fs_info,
	"raid56 write-intent log full, dropping record of %u stripes with failed writes at %llu, run scrub",
						      (unsigned int)hweight64(e->sticky),
						      e->bytenr);
			}
			atomic64_inc(&wib->stat_sticky_evicted);
			if (wib_entry_names_member(e))
				atomic64_inc(&wib->stat_stale_evicted);
			if (lost) {
				WRITE_ONCE(wib->replace_marks_lost, true);
				btrfs_raid56_alert(wib->fs_info, BTRFS_RAID56_EV_REPLACE_DROPPED,
						   e->bytenr, NULL, 0);
			}
			if (!lost || e->suspect_par || e->prior_par)
				btrfs_raid56_alert(wib->fs_info, BTRFS_RAID56_EV_DROPPED,
						   e->bytenr, NULL, 0);
			e->sticky = 0;
			if (e->stale) {
				atomic_sub(hweight64(e->stale), &wib->nr_stale);
				e->stale = 0;
			}
			wib_set_stale_par(wib, e, 0);
			wib_replace_forget(e);
			e->gen = 0;
			e->hold = 0;
			e->hold_par = 0;
			e->torn = 0;
			e->kept_torn = 0;
			return e;
		}
	}
	return NULL;
}

/*
 * Bring the live set back within what the current layout can express.
 *
 * Needed because the capacity does not only shrink when entries are added: the
 * moment the first stale bit appears it halves, so a log legally holding 120
 * regions a moment ago is now holding more than a block can describe, without
 * anything having been added.  Spend records until it fits -- wib_evict_sticky()
 * takes the vaguest first, so what goes is knowledge a scrub can rediscover by
 * reading, not the knowledge that names a member.
 *
 * Nothing may be evictable, every entry having a write in flight.  The set then
 * stays over the limit and the commit fails the write, which is the same answer
 * a full log already gives and is far better than writing a block that omits
 * stripes without saying so.
 */
static void wib_enforce_capacity_locked(struct btrfs_wib *wib)
{
	lockdep_assert_held(&wib->lock);
	wib_policy_locked(wib);

	while (wib_live_count(wib) > wib_live_max(wib)) {
		if (!wib_evict_sticky(wib))
			break;
	}
}

/*
 * Find the entry for @bytenr, or claim a free one, evicting a sticky-only
 * entry if @evict and nothing is free.  NULL if the log is full.
 */
static struct btrfs_wib_entry *wib_find_or_alloc_entry(struct btrfs_wib *wib,
						       u64 bytenr, bool evict)
{
	struct btrfs_wib_entry *free = NULL;

	lockdep_assert_held(&wib->lock);

	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		struct btrfs_wib_entry *e = &wib->entries[i];

		if (!wib_entry_used(e)) {
			if (!free)
				free = e;
			continue;
		}
		if (e->bytenr == bytenr)
			return e;
	}
	/*
	 * A free slot is not room if the block cannot describe it; evict
	 * instead, so the set stays expressible rather than growing into a
	 * state that cannot be written.
	 */
	if (free && wib_live_count(wib) >= wib_live_max(wib))
		free = NULL;
	if (!free && evict)
		free = wib_evict_sticky(wib);
	if (free) {
		free->bytenr = bytenr;
		free->sticky = 0;
		if (free->stale) {
			atomic_sub(hweight64(free->stale), &wib->nr_stale);
			free->stale = 0;
		}
		wib_set_stale_par(wib, free, 0);
		wib_replace_forget(free);
		free->gen = 0;
		free->hold = 0;
		free->hold_par = 0;
		free->torn = 0;
		free->kept_torn = 0;
	}
	return free;
}

/*
 * Number of entries [@logical, @logical + @len) needs that don't exist yet,
 * and the number of entries that could hold them (free ones, plus
 * sticky-only ones outside the range that can be evicted -- with every
 * device there, those that only say a write may have been torn only if
 * @spend_torn).
 */
static void wib_count_entries_locked(struct btrfs_wib *wib, u64 logical, u64 len,
				     bool spend_torn, unsigned int *needed,
				     unsigned int *avail)
{
	const u64 first = wib_entry_bytenr(logical);
	const u64 last = wib_entry_bytenr(logical + len - 1);

	lockdep_assert_held(&wib->lock);

	const u32 max = wib_live_max(wib);
	u32 live = 0, evictable = 0;

	*needed = 0;
	*avail = 0;
	for (u64 cur = first; cur <= last; cur += BTRFS_WIB_ENTRY_SIZE) {
		if (!wib_find_entry(wib, cur))
			(*needed)++;
	}
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		const struct btrfs_wib_entry *e = &wib->entries[i];

		if (!wib_entry_used(e))
			continue;
		live++;
		if (wib_evictable(wib, e, spend_torn) && (e->bytenr < first || e->bytenr > last))
			evictable++;
	}
	/*
	 * Free table slots are not the measure: a slot the current layout
	 * cannot describe is not room.  Evicting @evictable entries leaves
	 * @live - @evictable in use, so @max - @live + @evictable is what can
	 * still be admitted -- and it is negative, meaning nothing, when the
	 * set is already over the limit because a stale bit halved it.
	 */
	if (max + evictable > live)
		*avail = max + evictable - live;
}

/*
 * How long a write waits for room in a full log that only a repair can free.
 * A repair about to land after a device healed frees a slot within a second
 * or two; on a device that keeps failing, the queued repairs only retry on
 * their backoff and free nothing, and a longer wait would stall every write
 * that long before failing it.
 */
static unsigned int log_full_repair_wait_ms = 5000;
module_param_named(raid56_log_full_repair_wait_ms, log_full_repair_wait_ms, uint, 0644);
MODULE_PARM_DESC(raid56_log_full_repair_wait_ms,
		 "How long a RAID5/6 write waits for a repair to make room in a full write-intent log before failing (default 5000)");

/*
 * What can still make room in a full log?  A write in flight frees its slot
 * when it finishes; a queued or running repair retires the record it
 * repairs, if its device takes it.  With neither, waiting cannot help.
 */
enum wib_room {
	WIB_ROOM_NONE,
	WIB_ROOM_REPAIR,
	WIB_ROOM_WRITE,
};

static enum wib_room wib_room_source_locked(struct btrfs_wib *wib)
{
	lockdep_assert_held(&wib->lock);

	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++)
		if (wib->entries[i].bitmap)
			return WIB_ROOM_WRITE;
	if (atomic_read(&wib->repairs_inflight) || READ_ONCE(wib->repair_nr))
		return WIB_ROOM_REPAIR;
	return WIB_ROOM_NONE;
}

/*
 * Can room come without spending a record that only says a write may have been
 * torn?  From a repair, or from a write in flight into a region the log holds
 * no record of: its slot is free once it finishes.  A write into a recorded
 * region leaves the record, and frees nothing.
 */
static bool wib_room_without_torn_locked(struct btrfs_wib *wib)
{
	lockdep_assert_held(&wib->lock);

	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++)
		if (wib->entries[i].bitmap && !wib->entries[i].sticky)
			return true;
	return atomic_read(&wib->repairs_inflight) || READ_ONCE(wib->repair_nr);
}

/* Unless @spend_torn, see wib_room_without_torn_locked(). */
static bool wib_can_make_room(struct btrfs_wib *wib, bool spend_torn)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&wib->lock, flags);
	ret = spend_torn ? wib_room_source_locked(wib) != WIB_ROOM_NONE :
			   wib_room_without_torn_locked(wib);
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/* True if wib_try_mark_locked() for the same range would succeed. */
static bool wib_can_mark(struct btrfs_wib *wib, u64 logical, u64 len, bool spend_torn)
{
	unsigned long flags;
	unsigned int needed;
	unsigned int avail;

	spin_lock_irqsave(&wib->lock, flags);
	wib_policy_locked(wib);
	wib_count_entries_locked(wib, logical, len, spend_torn, &needed, &avail);
	spin_unlock_irqrestore(&wib->lock, flags);
	return needed <= avail;
}

/* True if btrfs_wib_try_mark() for the same range would succeed. */
bool btrfs_wib_can_mark(struct btrfs_wib *wib, u64 logical, u64 len)
{
	return wib_can_mark(wib, logical, len, true);
}

/*
 * Set the in-flight bits for [@logical, @logical + @len) without blocking,
 * spending a record that only says a write may have been torn for the room
 * only if @spend_torn (wib_entry_torn_only()).
 *
 * Return 0 on success, -ENOSPC if the log has no room for the entries the
 * range needs (nothing is modified in that case).
 */
static int wib_try_mark_locked(struct btrfs_wib *wib, u64 logical, u64 len,
			       bool spend_torn)
{
	const u64 end = logical + len;
	unsigned int needed;
	unsigned int avail;
	u64 cur;

	lockdep_assert_held(&wib->lock);

	wib_policy_locked(wib);
	wib_count_entries_locked(wib, logical, len, spend_torn, &needed, &avail);
	if (needed > avail)
		return -ENOSPC;

	/*
	 * Mark the existing entries first: an existing sticky-only entry of
	 * the range gets in-flight bits and can then not be evicted by the
	 * allocations below.
	 *
	 * And release @hold: a write starting now comes after any flush that
	 * failed while an earlier one was in flight, so whatever it lands the
	 * next flush covers, and the marks it clears when it completes are
	 * its to clear.
	 */
	for (cur = wib_entry_bytenr(logical); cur < end; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_entry *e = wib_find_entry(wib, cur);
		const u64 mask = btrfs_wib_range_mask(cur, logical, len);

		if (e) {
			e->bitmap |= mask;
			e->hold &= ~mask;
			e->hold_par &= ~mask;
		}
	}
	for (cur = wib_entry_bytenr(logical); cur < end; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_entry *e;

		if (wib_find_entry(wib, cur))
			continue;
		e = wib_find_or_alloc_entry(wib, cur, true);
		/*
		 * The counting above guarantees the room, and without
		 * @spend_torn it found it before wib_evict_sticky() reaches a
		 * record that only says a write may have been torn.
		 */
		ASSERT(e);
		e->bitmap |= btrfs_wib_range_mask(cur, logical, len);
	}
	return 0;
}

/*
 * wib_try_mark_locked() for a caller that cannot wait for room: a record that
 * only says a write may have been torn is spent before the log refuses.
 */
int btrfs_wib_try_mark(struct btrfs_wib *wib, u64 logical, u64 len)
{
	return wib_try_mark_locked(wib, logical, len, true);
}

/*
 * Two on-disk entry layouts exist: format 1 without the stale record, and the
 * current one with it.  The header's BTRFS_WIB_FLAG_STALE says which, and
 * these two are the only places the difference is allowed to matter -- every
 * other caller works on blocks this kernel built, which are always current.
 */
static bool wib_block_is_v1(const void *block)
{
	const struct btrfs_wib_disk_header *hdr = block;

	return !(le64_to_cpu(hdr->flags) & BTRFS_WIB_FLAG_STALE);
}

static u32 wib_block_max_entries(const void *block)
{
	return wib_block_is_v1(block) ? BTRFS_WIB_MAX_ENTRIES_V1 :
					BTRFS_WIB_MAX_ENTRIES;
}

/* Read entry @i of @block into @out, whichever layout the block is in. */
/* Write entry @i of @block in whichever layout its header declares. */
static void wib_write_entry(void *block, u32 i, const struct btrfs_wib_entry *e)
{
	struct btrfs_wib_disk_header *hdr = block;
	/*
	 * A block possibly torn is written as in flight: see @torn in struct
	 * btrfs_wib_entry.  An entry read back from a block has it in @bitmap
	 * already, and keeps it there when a union writes it again.
	 */
	const u64 bitmap = e->bitmap | (READ_ONCE(torn_no_persist) ? 0 : e->torn);

	if (wib_block_is_v1(block)) {
		struct btrfs_wib_disk_entry_v1 *de = block + sizeof(*hdr);

		de[i].bytenr = cpu_to_le64(e->bytenr);
		de[i].bitmap = cpu_to_le64(bitmap);
		de[i].error = cpu_to_le64(e->sticky);
	} else {
		struct btrfs_wib_disk_entry *de = block + sizeof(*hdr);

		de[i].bytenr = cpu_to_le64(e->bytenr);
		de[i].bitmap = cpu_to_le64(bitmap);
		de[i].error = cpu_to_le64(e->sticky);
		de[i].stale = cpu_to_le64(wib_no_persist() ? 0 : e->stale);
		de[i].stale_par = cpu_to_le64(wib_no_persist() ? 0 : e->stale_par);
		de[i].gen = cpu_to_le64(e->gen);
	}
}

/* Does @block carry any stale record at all? */
static bool wib_block_has_stale(const void *block)
{
	const struct btrfs_wib_disk_header *hdr = block;

	if (wib_block_is_v1(block))
		return false;
	for (u32 i = 0; i < le32_to_cpu(hdr->nr_entries); i++) {
		struct btrfs_wib_entry e;

		btrfs_wib_read_entry(block, i, &e);
		if (e.stale || e.stale_par)
			return true;
	}
	return false;
}
/*
 * Snapshot the in-flight set into @block (commit_mutex held).  If @base is
 * a valid block, everything it lists is kept listed as well (the result is
 * a superset of @base, so no flush is needed before writing it); -ENOSPC if
 * that does not fit.
 */
int btrfs_wib_build_block(struct btrfs_wib *wib, void *block, u64 seq, const void *base)
{
	unsigned long flags;
	struct btrfs_fs_info *fs_info = wib->fs_info;
	struct btrfs_wib_disk_header *hdr = block;
	u32 max, nr = 0;
	bool v2 = false;

	/*
	 * @base is unioned in below, after this memset has cleared @block.
	 * Aliasing them would zero the base and silently drop everything it
	 * lists.
	 */
	ASSERT(block != base);
	memset(block, 0, BTRFS_WIB_SLOT_SIZE);

	spin_lock_irqsave(&wib->lock, flags);
	/*
	 * Pick the narrowest layout that can say everything this block has to
	 * say, and pick it BEFORE writing anything, because the header flag
	 * that records the choice is also what wib_write_entry() and
	 * btrfs_wib_read_entry() key their stride off.
	 *
	 * Two reasons this is not simply "always write the wide one".  A block
	 * carrying the wide layout is refused outright by any kernel that
	 * predates it -- correctly, since it cannot know the stride -- and
	 * refusing a log means the stripes it covers are never recovered.
	 * Stamping the wide format on every block would impose that on every
	 * filesystem, including ones that have never had a stale record in
	 * their lives.  And the wide entry costs capacity that turns directly
	 * into device-wide cache flushes on the write path: 82 entries against
	 * 165 before the on-disk union overflows and btrfs_wib_mark() has to
	 * wait for a REQ_PREFLUSH to every device.
	 *
	 * Nothing stale is the overwhelmingly common case -- it is why
	 * btrfs_wib_stale() has a lock-free nr_stale == 0 fast path -- so the
	 * common case pays neither.
	 */
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		const struct btrfs_wib_entry *e = &wib->entries[i];

		/*
		 * The same test as wib_live_max(), or the set it admitted
		 * would not fit.  A wide block carries every mark, the
		 * recovery's verdicts included (@suspect_par).
		 */
		if (wib_entry_used(e) && wib_entry_wide(e)) {
			v2 = true;
			break;
		}
	}
	if (!v2 && base && le64_to_cpu(((const struct btrfs_wib_disk_header *)base)->magic)
			   == BTRFS_WIB_MAGIC && wib_block_has_stale(base))
		v2 = true;
	hdr->flags = v2 ? cpu_to_le64(BTRFS_WIB_FLAG_STALE) : 0;
	max = wib_block_max_entries(block);

	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		const struct btrfs_wib_entry *e = &wib->entries[i];

		if (!wib_entry_used(e))
			continue;
		if (nr == max) {
			spin_unlock_irqrestore(&wib->lock, flags);
			return -ENOSPC;
		}
		wib_write_entry(block, nr++, e);
	}
	spin_unlock_irqrestore(&wib->lock, flags);

	if (base) {
		const struct btrfs_wib_disk_header *bh = base;
		/*
		 * @base is always a block this kernel built -- wib->last or
		 * wib->prepared -- so its count is in range.  Nothing here
		 * enforces that though, and an out-of-range count would walk
		 * off the end of the slot, so clamp rather than trust the
		 * caller to stay disciplined.
		 */
		const u32 bnr = min_t(u32,
				      le64_to_cpu(bh->magic) == BTRFS_WIB_MAGIC ?
				      le32_to_cpu(bh->nr_entries) : 0,
				      wib_block_max_entries(base));

		for (u32 i = 0; i < bnr; i++) {
			struct btrfs_wib_entry be, cur;
			u32 j;

			btrfs_wib_read_entry(base, i, &be);
			for (j = 0; j < nr; j++) {
				btrfs_wib_read_entry(block, j, &cur);
				if (cur.bytenr == be.bytenr)
					break;
			}
			if (j == nr) {
				if (nr == max)
					return -ENOSPC;
				wib_write_entry(block, nr++, &be);
				continue;
			}
			cur.bitmap |= be.bitmap;
			cur.sticky |= be.sticky;
			cur.stale |= be.stale;
			cur.stale_par |= be.stale_par;
			cur.gen = max(cur.gen, be.gen);
			wib_write_entry(block, j, &cur);
		}
	}

	memcpy(hdr->fsid, fs_info->fs_devices->metadata_uuid, BTRFS_FSID_SIZE);
	hdr->magic = cpu_to_le64(BTRFS_WIB_MAGIC);
	hdr->seq = cpu_to_le64(seq);
	hdr->nr_entries = cpu_to_le32(nr);
	hdr->block_shift = cpu_to_le32(BTRFS_WIB_BLOCK_SHIFT);
	/*
	 * This kernel marks what may hide a torn write, whatever this block
	 * lists -- raid56_wf_torn_no_persist=1 included, which is a writer that
	 * marks and loses the marks.  See BTRFS_WIB_TRAILER_OFFSET.
	 */
	if (!READ_ONCE(log_unmarked))
		*(__le64 *)(block + BTRFS_WIB_TRAILER_OFFSET) =
			cpu_to_le64(BTRFS_WIB_TORN_MARKING);
	btrfs_csum(fs_info->csum_type, block + BTRFS_CSUM_SIZE,
		   BTRFS_WIB_SLOT_SIZE - BTRFS_CSUM_SIZE, hdr->csum);
	return 0;
}


void btrfs_wib_read_entry(const void *block, u32 i, struct btrfs_wib_entry *out)
{
	const struct btrfs_wib_disk_header *hdr = block;

	/*
	 * A possibly torn block is in @bitmap on disk, and a block written
	 * from this entry again (a union) writes @bitmap | @torn: nothing to
	 * decode, and nothing that may come from the caller's stack.
	 */
	out->torn = 0;
	if (wib_block_is_v1(block)) {
		const struct btrfs_wib_disk_entry_v1 *de = block + sizeof(*hdr);

		out->bytenr = le64_to_cpu(de[i].bytenr);
		out->bitmap = le64_to_cpu(de[i].bitmap);
		out->sticky = le64_to_cpu(de[i].error);
		/*
		 * Format 1 did not record which side of the stripe was wrong.
		 * Zero is the honest answer and the safe one: it means the
		 * scrub treats the stripe as merely suspect, which is what the
		 * kernel that wrote this block intended.
		 */
		out->stale = 0;
		out->stale_par = 0;
		out->gen = 0;
	} else {
		const struct btrfs_wib_disk_entry *de = block + sizeof(*hdr);

		out->bytenr = le64_to_cpu(de[i].bytenr);
		out->bitmap = le64_to_cpu(de[i].bitmap);
		out->sticky = le64_to_cpu(de[i].error);
		out->stale = le64_to_cpu(de[i].stale);
		out->stale_par = le64_to_cpu(de[i].stale_par);
		out->gen = le64_to_cpu(de[i].gen);
	}
}

/*
 * Did the writer of @block mark the records that may hide a torn write?  See
 * BTRFS_WIB_TRAILER_OFFSET.  Any other value there is a writer this kernel
 * does not know, and is read the way that loses nothing: as one that did not.
 */
bool btrfs_wib_block_marks_torn(const void *block)
{
	const __le64 *trailer = block + BTRFS_WIB_TRAILER_OFFSET;

	return le64_to_cpu(*trailer) == BTRFS_WIB_TORN_MARKING;
}

bool btrfs_wib_block_valid(const struct btrfs_fs_info *fs_info, const void *block)
{
	const struct btrfs_wib_disk_header *hdr = block;
	u8 csum[BTRFS_CSUM_SIZE];

	if (le64_to_cpu(hdr->magic) != BTRFS_WIB_MAGIC)
		return false;
	/*
	 * Written with metadata_uuid; also accept the fsid so that a
	 * btrfstune -m/-M between the crash and the mount doesn't hide the
	 * log.
	 */
	if (memcmp(hdr->fsid, fs_info->fs_devices->metadata_uuid, BTRFS_FSID_SIZE) != 0 &&
	    memcmp(hdr->fsid, fs_info->fs_devices->fsid, BTRFS_FSID_SIZE) != 0)
		return false;
	if (le32_to_cpu(hdr->block_shift) != BTRFS_WIB_BLOCK_SHIFT)
		return false;
	if (le64_to_cpu(hdr->flags) & ~BTRFS_WIB_FLAGS_SUPPORTED)
		return false;
	if (le32_to_cpu(hdr->nr_entries) > wib_block_max_entries(block))
		return false;
	/*
	 * Nothing sets these, and btrfs_wib_build_block() zeroes the whole
	 * slot, so a block that has them set was written by something this
	 * kernel does not understand.  Reject it rather than guess: ignoring a
	 * log leaves the stripes it covers unrecovered, which is exactly the
	 * behaviour without the feature, whereas misreading one could scrub
	 * the wrong stripes or silently skip the right ones.  The feature is
	 * compat_ro, so an older kernel will not have written this block --
	 * only a newer one with a format change will, which is the case this
	 * guards.  block_shift above, and the flags check, are for the same
	 * reason; flags is where a KNOWN format change announces itself, and
	 * BTRFS_WIB_FLAG_STALE is one, so that one is read rather than
	 * refused.  The trailer is not checked, on purpose: every value there
	 * can be read (btrfs_wib_block_marks_torn()).
	 */
	for (int i = 0; i < ARRAY_SIZE(hdr->reserved); i++)
		if (hdr->reserved[i] != 0)
			return false;
	btrfs_csum(fs_info->csum_type, block + BTRFS_CSUM_SIZE,
		   BTRFS_WIB_SLOT_SIZE - BTRFS_CSUM_SIZE, csum);
	if (memcmp(csum, hdr->csum, fs_info->csum_size) != 0)
		return false;
	/*
	 * Only now that the block is known to be intact, check what it says.
	 * Every user of an entry -- the union in btrfs_wib_build_block(), the
	 * lookup in wib_find_entry(), the bit arithmetic in
	 * btrfs_wib_range_mask() -- assumes the address is the base of an
	 * entry.  None of them can be made unsafe by an unaligned one
	 * (range_mask clamps to the entry and returns 0 on an empty
	 * intersection), but they would silently work on a different range
	 * than the one recorded, so recovery would scrub somewhere else and
	 * leave the real stripe alone.
	 */
	for (u32 i = 0; i < le32_to_cpu(hdr->nr_entries); i++) {
		struct btrfs_wib_entry e;

		btrfs_wib_read_entry(block, i, &e);
		if (!IS_ALIGNED(e.bytenr, BTRFS_WIB_ENTRY_SIZE))
			return false;
		/* An entry ending past the end of the address space. */
		if (e.bytenr > U64_MAX - BTRFS_WIB_ENTRY_SIZE)
			return false;
		/*
		 * @stale says the data of a block is not what was
		 * acknowledged, which is a statement about a block the log is
		 * already recording; outside @error it describes nothing, and
		 * acting on it would send a read to a parity that describes
		 * exactly the sector it is being told to distrust.
		 */
		if (e.stale & ~e.sticky)
			return false;
	}
	return true;
}

/*
 * These walk blocks this kernel built -- wib->last and wib->prepared -- which
 * used to make them stride-safe by construction.  They are not any more: a
 * block is written in the narrow layout whenever nothing is stale, so the same
 * kernel produces both, and every one of them has to decode rather than index.
 */
static u64 wib_entry_bits(const struct btrfs_wib_entry *e)
{
	return e->bitmap | e->sticky;
}

/* Return the bits of @oe (any kind) that @new no longer lists. */
static u64 wib_dropped_bits(const struct btrfs_wib_entry *oe, const void *new)
{
	const struct btrfs_wib_disk_header *nh = new;
	const u32 nnr = le32_to_cpu(nh->nr_entries);
	u64 bits = wib_entry_bits(oe);
	/*
	 * A stale bit going away has to count as a dropped bit in its own
	 * right, and cannot be folded into the OR above: @stale is a subset of
	 * @sticky, so bitmap|sticky|stale is just bitmap|sticky and a stale bit
	 * clearing on its own would look like no change at all.
	 *
	 * It has to count because of what clears it -- a data write that
	 * landed (rmw_update_stale_data()).  Persisting "no longer stale"
	 * before that write is durable would leave a log saying the sector on
	 * disk can be trusted while the disk still holds the old content, and
	 * the next scrub would then recompute the parity from it.  Treating it
	 * as dropped makes the commit flush first, which is the ordering the
	 * record needs.
	 */
	u64 stale = oe->stale;

	for (u32 j = 0; j < nnr && (bits || stale); j++) {
		struct btrfs_wib_entry ne;

		btrfs_wib_read_entry(new, j, &ne);
		if (ne.bytenr == oe->bytenr) {
			bits &= ~wib_entry_bits(&ne);
			stale &= ~ne.stale;
		}
	}
	return bits | stale;
}

/* Return true if any block listed in @old is not listed in @new. */
bool btrfs_wib_block_drops(const void *old, const void *new)
{
	const struct btrfs_wib_disk_header *oh = old;
	const u32 onr = le32_to_cpu(oh->nr_entries);

	if (le64_to_cpu(oh->magic) != BTRFS_WIB_MAGIC)
		return false;

	for (u32 i = 0; i < onr; i++) {
		struct btrfs_wib_entry oe;

		btrfs_wib_read_entry(old, i, &oe);
		if (wib_dropped_bits(&oe, new))
			return true;
	}
	return false;
}

static void wib_write_end_io(struct bio *bio)
{
	struct btrfs_wib *wib = bio->bi_private;

	/* The bio is inspected and freed by the submitter after the wait. */
	if (atomic_dec_and_test(&wib->io_pending))
		wake_up(&wib->io_wait);
}

/*
 * How many device failures the RAID56 profiles in use can tolerate.  Used to
 * decide how many copies of a log block must have been written.
 */
static int wib_max_tolerated_failures(struct btrfs_fs_info *fs_info)
{
	const u64 bits = fs_info->avail_data_alloc_bits |
			 fs_info->avail_metadata_alloc_bits |
			 fs_info->avail_system_alloc_bits;

	if (bits & BTRFS_BLOCK_GROUP_RAID6)
		return 2;
	return 1;
}

/*
 * Collect a reference to the block device file of every device the log has
 * to be written to, and the slot to write on each.
 *
 * This deliberately does not take device_list_mutex: the log is written from
 * the RMW worker while the RMW's bio holds the dev-replace bio counter, and
 * btrfs_dev_replace_finishing() waits for that counter to drain while
 * holding device_list_mutex.  The device list is traversed under RCU
 * instead; a device is only closed after a grace period following its
 * removal from the list (see btrfs_rm_device() and the dev-replace teardown
 * helpers), so the file reference taken here is always on an open file.
 */
static int wib_collect_targets(struct btrfs_fs_info *fs_info,
			       struct file ***files_ret, unsigned int **slots_ret,
			       int *nr_ret)
{
	struct btrfs_fs_devices *fs_devices = fs_info->fs_devices;
	struct btrfs_device *device;
	struct file **files;
	unsigned int *slots;
	int capacity;
	int nr;

	while (true) {
		rcu_read_lock();
		capacity = 0;
		list_for_each_entry_rcu(device, &fs_devices->devices, dev_list)
			capacity++;
		rcu_read_unlock();
		capacity += 4;

		files = kcalloc(capacity, sizeof(*files), GFP_NOFS);
		slots = kcalloc(capacity, sizeof(*slots), GFP_NOFS);
		if (!files || !slots) {
			kfree(files);
			kfree(slots);
			return -ENOMEM;
		}

		nr = 0;
		rcu_read_lock();
		list_for_each_entry_rcu(device, &fs_devices->devices, dev_list) {
			struct file *bdev_file;

			if (nr == capacity)
				break;
			if (!test_bit(BTRFS_DEV_STATE_IN_FS_METADATA, &device->dev_state) ||
			    !test_bit(BTRFS_DEV_STATE_WRITEABLE, &device->dev_state) ||
			    test_bit(BTRFS_DEV_STATE_MISSING, &device->dev_state))
				continue;
			/*
			 * Read once: the NULL check and the reference must see
			 * the same pointer.  btrfs_close_one_device() clears
			 * this field, and nothing here holds device_list_mutex
			 * against it, so re-reading it -- or letting the
			 * compiler do so -- would allow get_file(NULL).
			 */
			bdev_file = READ_ONCE(device->bdev_file);
			if (!bdev_file)
				continue;
			slots[nr] = READ_ONCE(device->wib_next_slot) % BTRFS_WIB_NR_SLOTS;
			files[nr++] = get_file(bdev_file);
		}
		rcu_read_unlock();

		if (nr < capacity)
			break;
		/* Devices were added meanwhile, retry with a larger array. */
		for (int i = 0; i < nr; i++)
			fput(files[i]);
		kfree(files);
		kfree(slots);
	}
	*files_ret = files;
	*slots_ret = slots;
	*nr_ret = nr;
	return 0;
}

static void wib_flush_failed_add(struct btrfs_wib_flush_failed *failed, u64 devid)
{
	for (unsigned int i = 0; i < failed->nr; i++)
		if (failed->devid[i] == devid)
			return;
	if (failed->nr < ARRAY_SIZE(failed->devid))
		failed->devid[failed->nr++] = devid;
	else
		failed->overflow = true;
}

static bool wib_flush_failed_any(const struct btrfs_wib_flush_failed *failed)
{
	return failed && (failed->nr || failed->overflow);
}

static void wib_flush_failed_merge(struct btrfs_wib_flush_failed *to,
				   const struct btrfs_wib_flush_failed *from)
{
	if (!from)
		return;
	to->overflow |= from->overflow;
	for (unsigned int i = 0; i < from->nr; i++)
		wib_flush_failed_add(to, from->devid[i]);
}

/* Is @dev one of the devices in @failed?  RCU read side: @dev is from a chunk map. */
static bool wib_flush_failed_dev(const struct btrfs_wib_flush_failed *failed,
				 const struct btrfs_device *dev)
{
	if (!dev)
		return false;
	if (failed->overflow)
		return true;
	for (unsigned int i = 0; i < failed->nr; i++)
		if (failed->devid[i] == dev->devid)
			return true;
	return false;
}

/* "devid 3", "devids 3 5", "every device", "a device" -- for the messages. */
static void wib_flush_failed_describe(const struct btrfs_wib_flush_failed *failed,
				      char *buf, size_t size)
{
	int len;

	if (failed->overflow || !failed->nr) {
		strscpy(buf, failed->overflow ? "every device" : "a device", size);
		return;
	}
	len = scnprintf(buf, size, "devid%s", failed->nr > 1 ? "s" : "");
	for (unsigned int i = 0; i < failed->nr; i++)
		len += scnprintf(buf + len, size - len, " %llu", failed->devid[i]);
}

/*
 * The devices whose barrier failed in this transaction commit, which is
 * running write_all_supers() under device_list_mutex: barrier_all_devices()
 * has just set BTRFS_DEV_STATE_FLUSH_FAILED on exactly those.
 */
static void wib_barrier_failed(struct btrfs_fs_info *fs_info,
			       struct btrfs_wib_flush_failed *failed)
{
	struct btrfs_device *device;

	memset(failed, 0, sizeof(*failed));
	rcu_read_lock();
	list_for_each_entry_rcu(device, &fs_info->fs_devices->devices, dev_list)
		if (test_bit(BTRFS_DEV_STATE_FLUSH_FAILED, &device->dev_state))
			wib_flush_failed_add(failed, device->devid);
	rcu_read_unlock();
}

/*
 * After a commit: advance the slot of every device that got the block, and
 * account the failed writes.  After a flush (!@with_data): add every device
 * that did not confirm it to @failed, which is what wib_readd_dropped() names
 * -- the log's own flush is the only thing that knows which device it was.
 * Devices removed meanwhile are simply skipped.
 */
static void wib_update_targets(struct btrfs_fs_info *fs_info, struct file **files,
			       const unsigned int *slots, const bool *ok, int nr,
			       bool with_data, struct btrfs_wib_flush_failed *failed)
{
	struct btrfs_device *device;

	rcu_read_lock();
	list_for_each_entry_rcu(device, &fs_info->fs_devices->devices, dev_list) {
		for (int i = 0; i < nr; i++) {
			if (device->bdev != file_bdev(files[i]))
				continue;
			if (!with_data) {
				if (!ok[i] && failed)
					wib_flush_failed_add(failed, device->devid);
			} else if (ok[i]) {
				WRITE_ONCE(device->wib_next_slot,
					   (slots[i] + 1) % BTRFS_WIB_NR_SLOTS);
			} else {
				btrfs_dev_stat_inc_and_print(device,
							     BTRFS_DEV_STAT_WRITE_ERRS);
			}
			break;
		}
	}
	rcu_read_unlock();
}

/*
 * Submit @bio to every target and wait; return the number of failures.  For a
 * flush, the devices that failed it are added to @failed if given.
 */
static int wib_submit_all_devices(struct btrfs_wib *wib, blk_opf_t opf, bool with_data,
				  int *nr_ret, struct btrfs_wib_flush_failed *failed)
{
	struct btrfs_fs_info *fs_info = wib->fs_info;
	struct page *page = virt_to_page(wib->block);
	struct file **files = NULL;
	unsigned int *slots = NULL;
	struct bio **bios;
	bool *ok;
	int nr = 0;
	int nr_errors = 0;
	int ret;

	*nr_ret = 0;
	ret = wib_collect_targets(fs_info, &files, &slots, &nr);
	if (ret < 0)
		return ret;
	*nr_ret = nr;
	if (nr == 0) {
		kfree(files);
		kfree(slots);
		return 0;
	}
	bios = kcalloc(nr, sizeof(*bios), GFP_NOFS);
	ok = kcalloc(nr, sizeof(*ok), GFP_NOFS);
	if (!bios || !ok) {
		ret = -ENOMEM;
		goto out;
	}

	atomic_set(&wib->io_pending, 1);
	for (int i = 0; i < nr; i++) {
		struct bio *bio;

		bio = bio_alloc(file_bdev(files[i]), with_data ? 1 : 0, opf, GFP_NOFS);
		bio->bi_private = wib;
		bio->bi_end_io = wib_write_end_io;
		if (with_data) {
			bio->bi_iter.bi_sector = (BTRFS_WIB_OFFSET +
						  slots[i] * BTRFS_WIB_SLOT_SIZE) >> SECTOR_SHIFT;
			__bio_add_page(bio, page, BTRFS_WIB_SLOT_SIZE, 0);
		}
		bios[i] = bio;
		atomic_inc(&wib->io_pending);
		submit_bio(bio);
	}
	if (!atomic_dec_and_test(&wib->io_pending))
		wait_event(wib->io_wait, atomic_read(&wib->io_pending) == 0);

	for (int i = 0; i < nr; i++) {
		ok[i] = bios[i]->bi_status == BLK_STS_OK;
		if (!ok[i]) {
			nr_errors++;
			btrfs_warn_rl(fs_info,
				"raid56 write-intent log %s failed on %pg: %d",
				with_data ? "write" : "flush", bios[i]->bi_bdev,
				blk_status_to_errno(bios[i]->bi_status));
		}
		bio_put(bios[i]);
	}
	if (with_data || (nr_errors && failed))
		wib_update_targets(fs_info, files, slots, ok, nr, with_data, failed);
	ret = nr_errors;
out:
	kfree(ok);
	kfree(bios);
	for (int i = 0; i < nr; i++)
		fput(files[i]);
	kfree(files);
	kfree(slots);
	return ret;
}

/*
 * Flush the write cache of every writable device.  Return 0 if every device
 * confirmed the flush (or barriers are disabled); the number of devices that
 * did not, which @failed names, if some did not; a negative error if the flush
 * could not be issued at all.
 */
static int wib_flush_all_devices(struct btrfs_wib *wib,
				 struct btrfs_wib_flush_failed *failed)
{
	int nr;

	memset(failed, 0, sizeof(*failed));
	if (btrfs_test_opt(wib->fs_info, NOBARRIER))
		return 0;
	atomic64_inc(&wib->stat_commit_flushes);
	/*
	 * A flush that could not even be submitted (-ENOMEM) names nobody: no
	 * device reported losing anything, the stripes only stay recorded.
	 */
	return wib_submit_all_devices(wib, REQ_OP_WRITE | REQ_PREFLUSH | REQ_SYNC, false,
				      &nr, failed);
}

/*
 * Write wib->block to the next slot of every writable device (FUA) and
 * wait.
 *
 * @nr_errors_ret receives the number of devices that failed the write.
 * Return 0 when enough copies were written for the block to survive the
 * tolerated number of device failures, -EIO otherwise.
 */
static int wib_write_all_devices(struct btrfs_wib *wib, int *nr_errors_ret)
{
	struct btrfs_fs_info *fs_info = wib->fs_info;
	blk_opf_t opf = REQ_OP_WRITE | REQ_SYNC | REQ_META | REQ_PRIO;
	int nr_errors;
	int nr;
	int ret;

	*nr_errors_ret = 0;
	if (!btrfs_test_opt(fs_info, NOBARRIER))
		opf |= REQ_FUA;

	ret = wib_submit_all_devices(wib, opf, true, &nr, NULL);
	if (ret < 0)
		return ret;
	nr_errors = ret;
	*nr_errors_ret = nr_errors;
	if (nr_errors == 0)
		return 0;

	/*
	 * The block must survive every further device loss the filesystem
	 * can still survive.  A device that is missing, or that has just
	 * failed this write, has already used up tolerance -- its data is
	 * not current either -- so with k of them the filesystem can lose
	 * t - k more, and t - k + 1 copies are needed.  The n - k devices
	 * that took the write always provide that while k <= t; past it the
	 * filesystem has nothing left to lose and one copy is all there is.
	 *
	 * Asking for t + 1 regardless, as this used to, made every recorded
	 * write fail while the filesystem was well inside its tolerance:
	 * metadata raid6 on three devices with one failing writes, or RAID6
	 * on four with two.
	 *
	 * Only ever a relaxation of that rule, never a tightening: past the
	 * tolerance (k > t) t + 1 copies still let writes that do not touch
	 * the failing devices go ahead, as they always did.
	 */
	if (nr - nr_errors >= 1 &&
	    (nr - nr_errors >= wib_max_tolerated_failures(fs_info) + 1 ||
	     nr_errors + (int)READ_ONCE(fs_info->fs_devices->missing_devices) <=
	     wib_max_tolerated_failures(fs_info))) {
		btrfs_warn_rl(fs_info,
		"raid56 write-intent log: %d of %d device writes failed, continuing",
			      nr_errors, nr);
		return 0;
	}
	btrfs_err_rl(fs_info,
	"raid56 write-intent log: %d of %d device writes failed, not enough copies",
		     nr_errors, nr);
	return -EIO;
}

/*
 * Testing only: re-add the stripes a failed flush keeps the way the log did
 * before it named anything -- as records that do not say which device, and
 * losing whatever does not fit.  See wib_readd_dropped().  The negative
 * control for tools/testing/btrfs/uml/readd_flush.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool readd_legacy;
module_param_named(raid56_wf_no_readd_name, readd_legacy, bool, 0644);
MODULE_PARM_DESC(raid56_wf_no_readd_name,
		 "Keep the stripes a failed flush left in the write-intent log without naming the device, losing those that do not fit (testing only: restores a known defect)");
#else
static const bool readd_legacy;
#endif

/*
 * Testing only: name a device that did not confirm a flush wherever the flush
 * covered its member -- every block the readd takes back, and every block with
 * a write in flight -- whether or not anything was written to it, and not in a
 * record some earlier failure made, as the readd did before it kept track of
 * what was written (struct btrfs_wib_written).  The negative control for the
 * name-control arm of tools/testing/btrfs/uml/rmw_cache.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool name_unwritten;
module_param_named(raid56_wf_name_unwritten, name_unwritten, bool, 0644);
MODULE_PARM_DESC(raid56_wf_name_unwritten,
		 "Name a device that failed a flush in every stripe the flush covered, written to or not (testing only: restores the old behaviour)");
#else
static const bool name_unwritten;
#endif

/*
 * Testing only.  raid56_wf_disable_forgets_writes=1: a disable forgets which
 * members were written before it writes the log's last block, and ignores its
 * commit's failed barrier, so a failed flush there names nothing -- see
 * wib_write_final_locked().
 *
 * raid56_wf_snapshot_misses_marks=1: only a mark's own lazy commit folds the
 * set into a transaction commit's snapshot, not the other writers of a block
 * that satisfies the mark -- see wib_fold_prepared_locked().
 *
 * raid56_wf_readd_admits_new=1: while a failed flush's readd is owed, record
 * writes into regions the last block does not list, and let the readd wait
 * for the room without a bound, as before either was limited -- see
 * @readd_admit_last in struct btrfs_wib.  The negative control for
 * uml/readd_admit.sh.
 *
 * raid56_wf_readd_no_repair=1: a readd that names a member queues no repair of
 * its full stripe and raises no alert, as before wib_readd_queue_repairs():
 * the records stay until a write into the stripe, a scrub or a mount.  The
 * negative control for the named arm of uml/flush_wedge.sh.
 *
 * raid56_wf_finished_stay_inflight=1: the log goes on listing in flight writes
 * that finished, as it did before wib_readd_base() and btrfs_wib_unmount():
 * the block written after a failed flush those that finished before the
 * flush was issued, although the readd took them back as the error records
 * that say what the flush may have lost, and an unmount the writes that
 * finished after the last commit -- so the next mount takes their stripes
 * for possibly torn, and refuses what a stripe with a named column needs
 * rebuilt from its only parity.  The negative control for the inflight arms
 * of uml/recover_scrub.sh and uml/readd_flush.sh.
 *
 * raid56_wf_readd_disowns_all=1: a readd takes every stale mark it carries
 * back from the last block as its own, as it did before it disowned only what
 * it names or adds (wib_readd_dropped()): a mark the set still holds loses
 * what it was -- the recovery's verdict on a stripe it could not decide
 * (@suspect_par, @prior_par), or a running replace's record of the zeros on
 * its target (@replace_stale) -- and a full log with a device missing spends
 * it in table order.  The negative control for the flush arm of
 * uml/verdict_keep.sh.
 *
 * raid56_wf_readd_admits_busy=1: while a readd is owed, record writes into a
 * region the set holds with nothing but writes in flight, which the last
 * block does not list, as before wib_readd_refuses_locked() matched what
 * wib_readd_waiting() waits for: writers that keep such a region busy hold
 * the readd's room until BTRFS_WIB_READD_WAIT, each of their writes failing,
 * and then records are lost.  The negative control for the hot arm of
 * uml/readd_admit.sh.
 *
 * raid56_wf_readd_says_each=1: every wait of a failed flush's readd for room
 * is said, and so is its end, as before wib_readd_say_wait(): while a device
 * keeps failing flushes under load, a pair of lines for each failed flush for
 * as long as it misbehaves.  The negative control for the say arm of
 * uml/readd_admit.sh.
 *
 * raid56_wf_remount_ro_keeps_inflight=1: a remount read-only leaves the log
 * listing in flight the writes that finished after the last transaction
 * commit, as before btrfs_wib_remount_ro(): nothing writes the log again
 * before a crash, or before the unmount, which writes nothing on a read-only
 * filesystem (close_ctree()) -- so the next mount takes their stripes for
 * possibly torn.  The negative control for the remount arm of
 * uml/recover_scrub.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool disable_forgets_writes;
module_param_named(raid56_wf_disable_forgets_writes, disable_forgets_writes, bool, 0644);
MODULE_PARM_DESC(raid56_wf_disable_forgets_writes,
		 "Forget which members were written before the write-intent log's last block is written at a disable, and ignore that commit's failed barrier (testing only: restores a known defect)");
static bool snapshot_misses_marks;
module_param_named(raid56_wf_snapshot_misses_marks, snapshot_misses_marks, bool, 0644);
MODULE_PARM_DESC(raid56_wf_snapshot_misses_marks,
		 "Let a transaction commit drop stripes whose record a log flush wrote after the commit's snapshot (testing only: restores a known defect)");
static bool readd_admits_new;
module_param_named(raid56_wf_readd_admits_new, readd_admits_new, bool, 0644);
MODULE_PARM_DESC(raid56_wf_readd_admits_new,
		 "While the write-intent log owes the records a failed flush kept, record writes into regions its last block does not list, and wait for the room without a bound (testing only: restores a known defect)");
static bool readd_no_repair;
module_param_named(raid56_wf_readd_no_repair, readd_no_repair, bool, 0644);
MODULE_PARM_DESC(raid56_wf_readd_no_repair,
		 "Queue no repair of the stripes a failed flush names in the write-intent log, and raise no alert for them (testing only: restores a known wedge)");
static bool finished_stay_inflight;
module_param_named(raid56_wf_finished_stay_inflight, finished_stay_inflight, bool, 0644);
MODULE_PARM_DESC(raid56_wf_finished_stay_inflight,
		 "Keep listing in flight in the write-intent log the writes that finished before a failed flush, although the readd recorded what the flush may have lost, and those that finished before an unmount, so the next mount takes their stripes for possibly torn (testing only: restores a known defect)");
static bool readd_disowns_all;
module_param_named(raid56_wf_readd_disowns_all, readd_disowns_all, bool, 0644);
MODULE_PARM_DESC(raid56_wf_readd_disowns_all,
		 "Let a failed flush's readd take over every stale mark it carries back, turning the recovery's verdicts and a running replace's marks into plain stale records a degraded full write-intent log spends in table order (testing only: restores a known defect)");
static bool readd_admits_busy;
module_param_named(raid56_wf_readd_admits_busy, readd_admits_busy, bool, 0644);
MODULE_PARM_DESC(raid56_wf_readd_admits_busy,
		 "While the write-intent log owes the records a failed flush kept, record writes into regions only writes in flight hold, which its last block does not list, so busy regions hold its room until it gives up and loses records (testing only: restores a known defect)");
static bool readd_says_each;
module_param_named(raid56_wf_readd_says_each, readd_says_each, bool, 0644);
MODULE_PARM_DESC(raid56_wf_readd_says_each,
		 "Say every wait of a failed flush's readd for room in the write-intent log and its end, one pair per failed flush (testing only: restores a known log flood)");
static bool remount_ro_keeps_inflight;
module_param_named(raid56_wf_remount_ro_keeps_inflight, remount_ro_keeps_inflight, bool, 0644);
MODULE_PARM_DESC(raid56_wf_remount_ro_keeps_inflight,
		 "Leave the write-intent log listing in flight the writes that finished before a remount read-only, so the next mount takes their stripes for possibly torn (testing only: restores a known defect)");
#else
static const bool disable_forgets_writes;
static const bool snapshot_misses_marks;
static const bool readd_admits_new;
static const bool readd_no_repair;
static const bool finished_stay_inflight;
static const bool readd_disowns_all;
static const bool readd_admits_busy;
static const bool readd_says_each;
static const bool remount_ro_keeps_inflight;
#endif

#ifdef CONFIG_BTRFS_FS_RUN_SANITY_TESTS
/* The self tests run at load, after the command line has set the knob. */
bool btrfs_wib_readd_legacy(void)
{
	return READ_ONCE(readd_legacy);
}

bool btrfs_wib_name_unwritten(void)
{
	return READ_ONCE(name_unwritten);
}

bool btrfs_wib_disable_forgets_writes(void)
{
	return READ_ONCE(disable_forgets_writes);
}

bool btrfs_wib_snapshot_misses_marks(void)
{
	return READ_ONCE(snapshot_misses_marks);
}

bool btrfs_wib_readd_admits_new(void)
{
	return READ_ONCE(readd_admits_new);
}

bool btrfs_wib_readd_no_repair(void)
{
	return READ_ONCE(readd_no_repair);
}

bool btrfs_wib_finished_stay_inflight(void)
{
	return READ_ONCE(finished_stay_inflight);
}

bool btrfs_wib_remount_ro_keeps_inflight(void)
{
	return READ_ONCE(remount_ro_keeps_inflight);
}

bool btrfs_wib_readd_disowns_all(void)
{
	return READ_ONCE(readd_disowns_all);
}

bool btrfs_wib_readd_admits_busy(void)
{
	return READ_ONCE(readd_admits_busy);
}
#endif

/* The written record of region @bytenr, or NULL.  Caller holds wib->lock. */
static struct btrfs_wib_written *wib_written_find(struct btrfs_wib *wib, u64 bytenr)
{
	lockdep_assert_held(&wib->lock);

	for (int i = 0; i < BTRFS_WIB_WRITTEN_SLOTS; i++) {
		struct btrfs_wib_written *w = &wib->written[i];

		if ((w->cols | w->par) && w->bytenr == bytenr)
			return w;
	}
	return NULL;
}

/*
 * The written record of region @bytenr, or a free one for it.  None free:
 * NULL, and from then on every member counts as written (@written_unknown),
 * which names more than it has to and never less.
 */
static struct btrfs_wib_written *wib_written_get(struct btrfs_wib *wib, u64 bytenr)
{
	struct btrfs_wib_written *free = NULL;

	lockdep_assert_held(&wib->lock);

	for (int i = 0; i < BTRFS_WIB_WRITTEN_SLOTS; i++) {
		struct btrfs_wib_written *w = &wib->written[i];

		if (!(w->cols | w->par)) {
			if (!free)
				free = w;
			continue;
		}
		if (w->bytenr == bytenr)
			return w;
	}
	if (!free) {
		if (!wib->written_unknown && !btrfs_is_testing(wib->fs_info))
			btrfs_warn_rl(wib->fs_info,
	"raid56 write-intent log: lost track of which members were written; a failed flush names every member it covers until the log is empty again");
		wib->written_unknown = true;
		return NULL;
	}
	free->bytenr = bytenr;
	return free;
}

/*
 * A write of the full stripe at @full_stripe_start is about to issue its bios
 * without FUA: to the data columns in @cols (bit c for column c) and to the
 * parities in @par (bit p for parity p), those it cannot issue for a missing
 * device included.  See struct btrfs_wib_written.
 *
 * @logged: btrfs_wib_mark() recorded the write, so every block of it is listed
 * in wib->last until a flush every device confirmed drops it.  A write the log
 * does not record -- a full stripe copy-on-write, a scrub's parity, a repair
 * writing back what it rebuilt (btrfs_wib_note_written_data()) -- is noted
 * only in a block the log records: nothing names a block the log does not
 * list, so there is nothing to keep for one.
 */
void btrfs_wib_note_written(struct btrfs_fs_info *fs_info, u64 full_stripe_start,
			    int nr_data, u64 cols, u32 par, bool logged)
{
	struct btrfs_wib *wib = fs_info->wib;
	unsigned long flags;

	if (!wib || !(cols | par))
		return;

	/* Parity p is addressed as the block of the stripe's start plus p. */
	cols &= nr_data < 64 ? BIT_ULL(nr_data) - 1 : U64_MAX;
	par &= 0x3;

	spin_lock_irqsave(&wib->lock, flags);
	/* Nothing names anything until it is; see btrfs_wib_enable(). */
	while (wib->enabled && (cols | par)) {
		const bool data = cols != 0;
		const int k = data ? __ffs64(cols) : __ffs(par);
		const u64 logical = full_stripe_start + ((u64)k << BTRFS_WIB_BLOCK_SHIFT);
		const u64 cur = wib_entry_bytenr(logical);
		const u64 bit = BIT_ULL((logical - cur) >> BTRFS_WIB_BLOCK_SHIFT);
		struct btrfs_wib_written *w;

		if (data)
			cols &= ~BIT_ULL(k);
		else
			par &= ~BIT(k);
		w = wib_written_find(wib, cur);
		if (!w && !logged) {
			const struct btrfs_wib_entry *e = wib_find_entry(wib, cur);

			if (!e || !(wib_entry_bits(e) & bit))
				continue;
		}
		if (!w)
			w = wib_written_get(wib, cur);
		if (!w)
			continue;
		if (data)
			w->cols |= bit;
		else
			w->par |= bit;
	}
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * A write without FUA of [@logical, @logical + @len) of a RAID5/6 data column
 * that no read-modify-write makes: a read repair, or a scrub writing back what
 * it rebuilt, each to the column's own device.  See btrfs_wib_note_written().
 */
void btrfs_wib_note_written_data(struct btrfs_fs_info *fs_info, u64 logical, u64 len)
{
	for (u64 cur = round_down(logical, BTRFS_WIB_BLOCK_SIZE); cur < logical + len;
	     cur += BTRFS_WIB_BLOCK_SIZE)
		btrfs_wib_note_written(fs_info, cur, 1, 0x1, 0, false);
}

static int wib_bits_cmp(const void *a, const void *b)
{
	const struct btrfs_wib_bits *ba = a;
	const struct btrfs_wib_bits *bb = b;

	if (ba->bytenr < bb->bytenr)
		return -1;
	if (ba->bytenr > bb->bytenr)
		return 1;
	return 0;
}

/*
 * The in-flight bits of every entry of @snapshot into wib->snapbits, sorted,
 * so that the written records can be checked against them under wib->lock
 * without decoding the block there.  commit_mutex held: @snapshot is one of
 * the log's own blocks, stable meanwhile.
 */
static void wib_load_snapbits(struct btrfs_wib *wib, const void *snapshot)
{
	const struct btrfs_wib_disk_header *hdr = snapshot;
	u32 nr = 0;

	lockdep_assert_held(&wib->commit_mutex);

	if (le64_to_cpu(hdr->magic) == BTRFS_WIB_MAGIC) {
		const u32 n = min(le32_to_cpu(hdr->nr_entries),
				  wib_block_max_entries(snapshot));

		for (u32 i = 0; i < n && nr < BTRFS_WIB_NR_ENTRIES; i++) {
			struct btrfs_wib_entry e;

			btrfs_wib_read_entry(snapshot, i, &e);
			if (!e.bitmap)
				continue;
			wib->snapbits[nr].bytenr = e.bytenr;
			wib->snapbits[nr++].bits = e.bitmap;
		}
	}
	sort(wib->snapbits, nr, sizeof(*wib->snapbits), wib_bits_cmp, NULL);
	wib->nr_snapbits = nr;
}

/* The blocks of region @bytenr the loaded snapshot has a write in flight in. */
static u64 wib_snapbits(const struct btrfs_wib *wib, u64 bytenr)
{
	const struct btrfs_wib_bits key = { .bytenr = bytenr };
	const struct btrfs_wib_bits *b;

	b = bsearch(&key, wib->snapbits, wib->nr_snapbits, sizeof(key), wib_bits_cmp);
	return b ? b->bits : 0;
}

/*
 * Every device confirmed a flush issued after @snapshot was taken.  A block
 * with no write in flight then had every write of its members completed
 * before the flush, so they are on stable media: forget them.  A block with
 * one may have written during the flush, and keeps what it has.
 *
 * A write the log records can only have been issued after its record was
 * durable, which for a block the snapshot does not have in flight means
 * before it was taken: every block written between a transaction commit's
 * snapshot and its drop folds the set, and with it every mark that block
 * satisfies, into that snapshot (wib_fold_prepared_locked()), and the log's
 * own flushes hold commit_mutex from their snapshot to their drop, so no mark
 * is satisfied meanwhile.
 */
static void wib_written_flushed(struct btrfs_wib *wib, const void *snapshot)
{
	unsigned long flags;

	lockdep_assert_held(&wib->commit_mutex);

	wib_load_snapbits(wib, snapshot);
	spin_lock_irqsave(&wib->lock, flags);
	for (int i = 0; i < BTRFS_WIB_WRITTEN_SLOTS; i++) {
		struct btrfs_wib_written *w = &wib->written[i];
		u64 keep;

		if (!(w->cols | w->par))
			continue;
		keep = wib_snapbits(wib, w->bytenr);
		w->cols &= keep;
		w->par &= keep;
	}
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * Forget every written record; the log is not enabled (any more).  A readd
 * still owed then has nothing left to say what was written to when the log is
 * next enabled and it plans again, and names every member the failed flush
 * covered (@written_unknown): more than it has to, never less.  Caller holds
 * commit_mutex and wib->lock.
 */
static void wib_written_reset_locked(struct btrfs_wib *wib)
{
	lockdep_assert_held(&wib->commit_mutex);
	lockdep_assert_held(&wib->lock);
	memset(wib->written, 0, BTRFS_WIB_WRITTEN_SLOTS * sizeof(*wib->written));
	wib->written_unknown = wib->readd_owed && !READ_ONCE(disable_forgets_writes);
}

/*
 * Which blocks of each entry of wib->last lie on a device in @failed, into
 * wib->readd_names[]: @stale where the block's data column is on one, and
 * @stale_par where the block is the p-th of its full stripe and parity p is on
 * one -- bit (b + p), the way the record addresses a parity.  Every block of
 * an entry belongs to exactly one full stripe, so both are a property of the
 * block alone and never spill into the next entry.
 *
 * The columns rotate by one device per full stripe, as btrfs_map_block() lays
 * them out: column c of the r-th full stripe of a chunk is on
 * map->stripes[(c + r) % num_stripes], the data columns first, then P and Q.
 *
 * Outside wib->lock, a spinlock held with interrupts off, and under
 * memalloc_nofs_save(), since this runs inside a transaction commit.
 * wib->last is stable meanwhile: it only changes under commit_mutex, which the
 * caller holds.  A block whose chunk has gone is not named: nothing reads it
 * through this record any more.
 */
static void wib_name_devices(struct btrfs_wib *wib,
			     const struct btrfs_wib_flush_failed *failed)
{
	struct btrfs_fs_info *fs_info = wib->fs_info;
	const struct btrfs_wib_disk_header *oh = wib->last;
	struct btrfs_chunk_map *map = NULL;
	unsigned int nofs_flag;
	u32 onr;

	lockdep_assert_held(&wib->commit_mutex);

	memset(wib->readd_names, 0, sizeof(wib->readd_names));
	if (!wib_flush_failed_any(failed) || le64_to_cpu(oh->magic) != BTRFS_WIB_MAGIC)
		return;
	onr = min(le32_to_cpu(oh->nr_entries), wib_block_max_entries(wib->last));

	nofs_flag = memalloc_nofs_save();
	for (u32 i = 0; i < onr; i++) {
		struct btrfs_wib_names *names = &wib->readd_names[i];
		struct btrfs_wib_entry old;
		u64 bits;

		btrfs_wib_read_entry(wib->last, i, &old);
		bits = wib_entry_bits(&old);
		while (bits) {
			const int j = __ffs64(bits);
			const u64 logical = old.bytenr + ((u64)j << BTRFS_WIB_BLOCK_SHIFT);
			const struct btrfs_device *dev;
			u32 stripe_nr, nr_data, rot, col;

			bits &= bits - 1;
			if (map && (logical < map->start ||
				    logical >= map->start + map->chunk_len)) {
				btrfs_free_chunk_map(map);
				map = NULL;
			}
			if (!map) {
				map = btrfs_find_chunk_map(fs_info, logical, BTRFS_WIB_BLOCK_SIZE);
				if (!map)
					continue;
			}
			if (!(map->type & BTRFS_BLOCK_GROUP_RAID56_MASK) || logical < map->start)
				continue;
			nr_data = nr_data_stripes(map);
			stripe_nr = (logical - map->start) >> BTRFS_STRIPE_LEN_SHIFT;
			rot = stripe_nr / nr_data;
			col = stripe_nr % nr_data;

			rcu_read_lock();
			dev = map->stripes[(col + rot) % map->num_stripes].dev;
			if (wib_flush_failed_dev(failed, dev)) {
				names->stale |= BIT_ULL(j);
				if (test_bit(BTRFS_DEV_STATE_MISSING, &dev->dev_state))
					names->absent |= BIT_ULL(j);
			}
			if (col < map->num_stripes - nr_data) {
				dev = map->stripes[(nr_data + col + rot) % map->num_stripes].dev;
				if (wib_flush_failed_dev(failed, dev)) {
					names->stale_par |= BIT_ULL(j);
					if (test_bit(BTRFS_DEV_STATE_MISSING, &dev->dev_state))
						names->absent |= BIT_ULL(j);
				}
			}
			rcu_read_unlock();
		}
	}
	if (map)
		btrfs_free_chunk_map(map);
	memalloc_nofs_restore(nofs_flag);
}

/*
 * How long an owed readd waits for the writes in flight that hold its room
 * before it takes back what fits and loses the rest (wib_readd_dropped()).
 * No new write takes more of the room meanwhile, nor keeps a region it waits
 * for busy (@readd_admit_last in struct btrfs_wib), so only writes stuck on a
 * device keep it waiting this long -- and every recorded write fails while it
 * does.  Shorter than BTRFS_WIB_FULL_TIMEOUT, which a write waiting on it gets.
 */
#define BTRFS_WIB_READD_WAIT		(30 * HZ)

/* How often an owed readd's wait for room is said: see wib_readd_say_wait(). */
#define BTRFS_WIB_READD_SAY_EVERY	(60 * HZ)

static int wib_u64_cmp(const void *a, const void *b)
{
	const u64 x = *(const u64 *)a;
	const u64 y = *(const u64 *)b;

	return x < y ? -1 : x > y;
}

/* Does the last block list region @bytenr?  Caller holds wib->lock. */
static bool wib_readd_lists(const struct btrfs_wib *wib, u64 bytenr)
{
	lockdep_assert_held(&wib->lock);
	return bsearch(&bytenr, wib->readd_last, wib->nr_readd_last, sizeof(u64),
		       wib_u64_cmp) != NULL;
}

/*
 * Set wib->readd_owed, and the part of it btrfs_wib_mark() looks at under
 * wib->lock: @readd_admit_last in struct btrfs_wib.  commit_mutex held, and
 * wib->last is the block the readd planned against.
 */
static void wib_readd_set_owed(struct btrfs_wib *wib, bool owed)
{
	const struct btrfs_wib_disk_header *oh = wib->last;
	unsigned long flags;

	lockdep_assert_held(&wib->commit_mutex);

	spin_lock_irqsave(&wib->lock, flags);
	if (owed && !wib->readd_owed)
		wib->readd_until = jiffies + BTRFS_WIB_READD_WAIT;
	wib->readd_admit_last = owed && !READ_ONCE(readd_admits_new);
	wib->nr_readd_last = 0;
	if (wib->readd_admit_last && le64_to_cpu(oh->magic) == BTRFS_WIB_MAGIC) {
		const u32 onr = min(le32_to_cpu(oh->nr_entries),
				    wib_block_max_entries(wib->last));

		for (u32 i = 0; i < onr && i < BTRFS_WIB_NR_ENTRIES; i++) {
			struct btrfs_wib_entry old;

			btrfs_wib_read_entry(wib->last, i, &old);
			wib->readd_last[wib->nr_readd_last++] = old.bytenr;
		}
		sort(wib->readd_last, wib->nr_readd_last, sizeof(u64), wib_u64_cmp, NULL);
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	/* A write waiting for the readd can go ahead now. */
	if (wib->readd_owed && !owed)
		wake_up_all(&wib->wait);
	wib->readd_owed = owed;
}

/*
 * Would a write into [@logical, @logical + @len) add a region to the block an
 * owed readd has to write, or keep one there that the readd waits to see go?
 * See @readd_admit_last in struct btrfs_wib.  Only a region the last block
 * lists, or one the set holds a record of, is in that block whatever happens;
 * one the set holds with nothing but writes in flight is exactly what
 * wib_readd_waiting() waits for.  A write recorded there cannot commit before
 * the readd is done, and fails -- and writers keeping such a region busy used
 * to hold the readd's room until BTRFS_WIB_READD_WAIT ran out and records were
 * lost (raid56_wf_readd_admits_busy=1).  Caller holds wib->lock.
 */
static bool wib_readd_refuses_locked(struct btrfs_wib *wib, u64 logical, u64 len)
{
	lockdep_assert_held(&wib->lock);

	if (!wib->readd_admit_last || !wib->enabled)
		return false;
	for (u64 cur = wib_entry_bytenr(logical); cur < logical + len;
	     cur += BTRFS_WIB_ENTRY_SIZE) {
		const struct btrfs_wib_entry *e = wib_find_entry(wib, cur);

		if (wib_readd_lists(wib, cur))
			continue;
		if (!e || !(e->sticky || READ_ONCE(readd_admits_busy)))
			return true;
	}
	return false;
}

bool btrfs_wib_readd_refuses(struct btrfs_wib *wib, u64 logical, u64 len)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&wib->lock, flags);
	ret = wib_readd_refuses_locked(wib, logical, len);
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/*
 * Is the room an owed readd needs held by a write in flight into a region the
 * last block does not list?  Those are what it waits for: once they are gone,
 * the set holds nothing the last block does not but records, and a readd that
 * found no room for them never would (wib_readd_dropped()).
 */
static bool wib_readd_waiting(struct btrfs_wib *wib)
{
	unsigned long flags;
	bool ret = false;

	spin_lock_irqsave(&wib->lock, flags);
	if (wib->readd_admit_last && wib->enabled) {
		for (int i = 0; !ret && i < BTRFS_WIB_NR_ENTRIES; i++) {
			const struct btrfs_wib_entry *e = &wib->entries[i];

			ret = e->bitmap && !e->sticky &&
			      !wib_readd_lists(wib, e->bytenr);
		}
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/*
 * The readd as it was before it named anything: the stripes a failed flush
 * keeps come back as records that do not say which device, and whatever the
 * in-memory set has no room for is lost -- the next commit whose flush
 * succeeds drops it.  Only for raid56_wf_no_readd_name=1.
 */
static void wib_readd_legacy(struct btrfs_wib *wib)
{
	unsigned long flags;
	const struct btrfs_wib_disk_header *oh = wib->last;
	const u32 onr = le32_to_cpu(oh->nr_entries);
	unsigned int nr_readded = 0;
	unsigned int nr_lost = 0;

	lockdep_assert_held(&wib->commit_mutex);

	wib_readd_set_owed(wib, false);
	memset(&wib->readd_owed_failed, 0, sizeof(wib->readd_owed_failed));
	if (le64_to_cpu(oh->magic) != BTRFS_WIB_MAGIC)
		return;

	spin_lock_irqsave(&wib->lock, flags);
	for (u32 i = 0; i < onr; i++) {
		struct btrfs_wib_entry old;
		struct btrfs_wib_entry *e;
		u64 bytenr, bits;

		btrfs_wib_read_entry(wib->last, i, &old);
		bytenr = old.bytenr;
		bits = wib_entry_bits(&old);
		e = wib_find_entry(wib, bytenr);
		if (e)
			bits &= ~(e->bitmap | e->sticky);
		if (!bits)
			continue;
		if (!e)
			e = wib_find_or_alloc_entry(wib, bytenr, false);
		if (!e) {
			nr_lost += hweight64(bits);
			continue;
		}
		e->sticky |= bits;
		e->gen = max(e->gen, wib->fs_info->generation);
		nr_readded += hweight64(bits);
		/* Carry the stale record back with it, see wib_readd_dropped(). */
		{
			/* Keep the invariant that @stale is a subset of @sticky. */
			const u64 add = (old.stale & e->sticky) & ~e->stale;

			wib_replace_disown(e, old.stale, old.stale_par);
			e->stale |= add;
			wib_set_stale_par(wib, e, e->stale_par | old.stale_par);
			if (add)
				atomic_add(hweight64(add), &wib->nr_stale);
		}
	}
	spin_unlock_irqrestore(&wib->lock, flags);

	if (nr_readded)
		atomic64_add(nr_readded, &wib->stat_sticky);
	if (btrfs_is_testing(wib->fs_info))
		return;
	if (nr_readded)
		btrfs_warn_rl(wib->fs_info,
	"raid56 write-intent log: keeping %u stripes recorded, a device did not confirm their data is flushed",
			      nr_readded);
	if (nr_lost) {
		btrfs_warn_rl(wib->fs_info,
	"raid56 write-intent log full, %u stripes whose flush a device did not confirm are not recorded, run scrub",
			      nr_lost);
		btrfs_raid56_alert(wib->fs_info, BTRFS_RAID56_EV_DROPPED, 0, NULL, 0);
	}
}

/*
 * The names of entry @i of the last block that a readd puts on, into @stale
 * and @stale_par: where its member lies on a device that failed a flush
 * (wib->readd_names[]) and, unless !@precise (raid56_wf_name_unwritten), a
 * write went to it (struct btrfs_wib_written).  Caller holds wib->lock.
 */
static void wib_readd_names(struct btrfs_wib *wib, u32 i, u64 bytenr, bool precise,
			    u64 *stale, u64 *stale_par)
{
	const struct btrfs_wib_names *names = &wib->readd_names[i];
	const struct btrfs_wib_written *w;

	lockdep_assert_held(&wib->lock);

	*stale = names->stale;
	*stale_par = names->stale_par;
	if (!precise || wib->written_unknown)
		return;
	w = wib_written_find(wib, bytenr);
	*stale &= w ? w->cols : 0;
	*stale_par &= w ? w->par : 0;
}

/*
 * Ask for a repair of every full stripe the readd just named a member of on a
 * device that is there (@repair in struct btrfs_wib_names).  A record naming a
 * member is not spent while every device is there (wib_may_evict_naming()),
 * and nothing else retires one until a write reaches its stripe, a scrub or a
 * mount: a failed flush naming more than the log holds besides would leave
 * every write into a new region failing, with nobody told why.  A repair
 * rebuilds the member from the rest of the stripe, and once it lands the
 * record goes (raid56_repair_finished()).  Return the first full stripe asked
 * for, 0 if none.  commit_mutex held; not wib->lock, the chunk map is looked
 * up here.
 */
static u64 wib_readd_queue_repairs(struct btrfs_wib *wib, u32 onr)
{
	struct btrfs_fs_info *fs_info = wib->fs_info;
	struct btrfs_chunk_map *map = NULL;
	unsigned int nofs_flag;
	u64 first = 0;
	u64 prev = 0;

	lockdep_assert_held(&wib->commit_mutex);

	nofs_flag = memalloc_nofs_save();
	for (u32 i = 0; i < onr; i++) {
		u64 bits = wib->readd_names[i].repair;
		struct btrfs_wib_entry old;

		if (!bits)
			continue;
		btrfs_wib_read_entry(wib->last, i, &old);
		while (bits) {
			const u64 logical = old.bytenr +
					    ((u64)__ffs64(bits) << BTRFS_WIB_BLOCK_SHIFT);
			u64 full_stripe_len;
			u64 start;

			bits &= bits - 1;
			if (map && (logical < map->start ||
				    logical >= map->start + map->chunk_len)) {
				btrfs_free_chunk_map(map);
				map = NULL;
			}
			if (!map) {
				map = btrfs_find_chunk_map(fs_info, logical, BTRFS_WIB_BLOCK_SIZE);
				if (!map)
					continue;
			}
			if (!(map->type & BTRFS_BLOCK_GROUP_RAID56_MASK) || logical < map->start)
				continue;
			full_stripe_len = (u64)nr_data_stripes(map) << BTRFS_STRIPE_LEN_SHIFT;
			start = map->start + div64_u64(logical - map->start, full_stripe_len) *
					     full_stripe_len;
			if (start == prev)
				continue;
			prev = start;
			if (!first)
				first = start;
			btrfs_raid56_queue_repair(fs_info, start, 0);
		}
	}
	if (map)
		btrfs_free_chunk_map(map);
	memalloc_nofs_restore(nofs_flag);
	return first;
}

/* What wib_readd_dropped() does with the records of the last block. */
enum wib_readd_plan {
	/* Take every record back, naming the devices wherever a name falls. */
	WIB_READD_NAMED,
	/* Take every record back without the names: no block can hold them. */
	WIB_READD_UNNAMED,
	/* Take nothing back yet: writes in flight hold the room, and finish. */
	WIB_READD_WAIT,
	/* Take back what fits and lose the rest: nothing will make the room. */
	WIB_READD_LOSE,
};

/*
 * An owed readd's wait for room begins (wib_readd_dropped()), failed by @who.
 * While a device keeps failing flushes under load, every failed flush begins
 * one and each is over within milliseconds: said every time, with its end, the
 * pair repeated at the failed-flush rate for as long as the device misbehaved,
 * scrolling everything else out of the kernel log.  So say one in
 * BTRFS_WIB_READD_SAY_EVERY, with how many went unsaid since, and its end only
 * when its beginning was said (@readd_said).  What the failed flushes cost is
 * said on its own, and the device's flush errors count in its statistics.
 */
static void wib_readd_say_wait(struct btrfs_wib *wib, const char *who)
{
	char more[64] = "";

	lockdep_assert_held(&wib->commit_mutex);

	if (READ_ONCE(readd_says_each)) {
		wib->readd_said = true;
		btrfs_warn_rl(wib->fs_info,
	"raid56 write-intent log: %s did not confirm a flush, and the log has no room to take back the stripes its last block lists until writes in flight finish; dropping nothing from it until then",
			      who);
		return;
	}
	wib->readd_said = __ratelimit(&wib->readd_say_rs);
	if (!wib->readd_said) {
		wib->readd_unsaid++;
		return;
	}
	if (wib->readd_unsaid)
		scnprintf(more, sizeof(more), " (and %u more times since this was last said)",
			  wib->readd_unsaid);
	wib->readd_unsaid = 0;
	btrfs_warn(wib->fs_info,
	"raid56 write-intent log: %s did not confirm a flush, and the log has no room to take back the stripes its last block lists until writes in flight finish; dropping nothing from it until then%s",
		   who, more);
}

/*
 * A commit is about to drop the stripes the last block lists but the
 * in-memory set does not, and some device did not confirm the flush of its
 * cache: that device may hold their data unflushed, so a block that no
 * longer lists them must not be written.  Keep them as error records (a
 * device may end up with stale sectors); they are scrubbed at the next
 * mount.
 *
 * And say WHICH device.  An error record alone says that something in the
 * stripe went wrong, not which side, and for data without a checksum that is
 * not enough to put it right: after a crash the device that lost its cache
 * hands back the old content of the column the write changed, the parity the
 * other devices flushed describes the new one, and a recovery that cannot
 * tell which of the two is stale has to leave the stripe alone -- a read then
 * returns the old data as the file's content, with no error anywhere.  So
 * the column of each device in @failed, or its parity (stale_par), is marked
 * stale in the blocks the failed flush was covering, exactly as a write that
 * device had failed outright would have left them -- but only where a write
 * went to that member since the device last confirmed a flush (struct
 * btrfs_wib_written).  What nothing wrote to is what the device held already,
 * and naming it anyway costs reads: a stripe with an earlier failure named on
 * another device then has two stale members under one parity, undecidable,
 * and a read of the one the parity rebuilds exactly is refused.  Where a write
 * did go, in any block the last block lists:
 *
 *  - the blocks this readd takes back into @sticky, which the commit was
 *    about to drop: their writes finished before the flush;
 *  - the blocks with a write in flight.  A write that finished before the
 *    flush may be hiding behind a newer one to the same stripe, which marked
 *    it again; the newer one may have landed part of itself before the flush
 *    failed; and once it completes its bits simply go, and the next flush
 *    every device confirms drops the stripe.  So the blocks a name falls on
 *    become @sticky as well, and @hold keeps the in-flight write from clearing
 *    the names when it completes;
 *  - the blocks already @sticky, from an earlier failure, that a write went to
 *    since: that write is as lost as any other.  A member of such a record
 *    that nothing wrote to since may be long settled on the device, and is
 *    not named.
 *
 * raid56_wf_name_unwritten=1 names as this did before it knew what was
 * written: every block taken back or with a write in flight, never one already
 * @sticky.
 *
 * @snapshot: a flush was issued after it was taken, and every device not in
 * @failed (nor owed a readd) confirmed it: whatever went to their members in a
 * block with no write in flight in it is on stable media, and is forgotten
 * first.  NULL when no flush was issued, or none that can be timed.
 *
 * wib_name_devices() does the chunk-map lookups before the lock is taken.
 *
 * And never lose one if anything can avoid it, which takes planning against
 * the block this commit actually writes: the in-memory set PLUS every record
 * of the last block.  A name, or a stale record carried back, puts that block
 * in the wide layout, which describes half as many regions (wib_live_max());
 * a block no layout can describe is one no commit can write.  So:
 *
 *  - it fits with the names: take everything back and name it;
 *  - it fits only without them: take everything back without the names, and
 *    say so (the log_flush_unnamed alert).  Records first, names second -- a
 *    record without the name still gets the stripe scrubbed and checked, a
 *    name without its record is nothing.  What the names would have said is
 *    kept in the one form that costs no room: the blocks are marked possibly
 *    torn (@torn in struct btrfs_wib_entry), which the block carries as in
 *    flight, so that a mount after a crash does not take them for plain
 *    failed writes whose damage the record names.  Waiting for the writes in
 *    flight would not bring the names back: the block this commit writes
 *    fits, and takes their records into the last block with it;
 *  - it does not fit at all, but will once the writes in flight that the last
 *    block does not list are gone -- no commit can write their records
 *    meanwhile, so each fails before it writes anything: take nothing back
 *    yet, and owe the readd (wib->readd_owed).  Every drop comes through here
 *    instead and plans again, until one of the cases above holds.  No new
 *    write into such a region is recorded meanwhile; each waits for this
 *    (@readd_admit_last in struct btrfs_wib).  If those writes are still
 *    there after BTRFS_WIB_READD_WAIT, lose instead, as below;
 *  - it can never fit, because the records the last block lists and those
 *    only the set has are more than the layout they force can describe: take
 *    back what fits and lose the rest, loudly, as the log did before -- the
 *    only alternative is a log that never writes again.
 *
 * Return true if nothing is owed any more.
 */
static bool wib_readd_dropped(struct btrfs_wib *wib,
			      const struct btrfs_wib_flush_failed *failed,
			      const void *snapshot)
{
	struct btrfs_fs_info *fs_info = wib->fs_info;
	const struct btrfs_wib_disk_header *oh = wib->last;
	const bool was_owed = wib->readd_owed;
	const bool precise = !READ_ONCE(name_unwritten);
	DECLARE_BITMAP(in_last, BTRFS_WIB_NR_ENTRIES);
	enum wib_readd_plan plan;
	unsigned int nr_readded = 0;
	unsigned int nr_named = 0;
	unsigned int nr_unnamed = 0;
	unsigned int nr_lost = 0;
	unsigned long flags;
	char who[64];
	bool forced_wide;
	bool naming = false;
	bool identified;
	bool gave_up;
	bool name;
	u32 nr_union;
	u32 nr_perm;
	u32 limit;
	u32 cap;
	u32 live;
	u32 onr;

	lockdep_assert_held(&wib->commit_mutex);

	/* Worked out below, for the records taken back; see wib_readd_base(). */
	wib->readd_landed = false;
	if (READ_ONCE(readd_legacy)) {
		wib_readd_legacy(wib);
		return true;
	}
	if (le64_to_cpu(oh->magic) != BTRFS_WIB_MAGIC) {
		/* Nothing was ever written, so nothing can be owed. */
		wib_readd_set_owed(wib, false);
		memset(&wib->readd_owed_failed, 0, sizeof(wib->readd_owed_failed));
		return true;
	}
	onr = min(le32_to_cpu(oh->nr_entries), wib_block_max_entries(wib->last));

	/* Everyone who failed a flush since the set last held everything. */
	wib_flush_failed_merge(&wib->readd_owed_failed, failed);
	identified = wib_flush_failed_any(&wib->readd_owed_failed);
	wib_name_devices(wib, &wib->readd_owed_failed);
	wib_flush_failed_describe(&wib->readd_owed_failed, who, sizeof(who));
	if (snapshot)
		wib_load_snapbits(wib, snapshot);

	spin_lock_irqsave(&wib->lock, flags);
	/*
	 * What the flush did make durable is not in doubt: every member of a
	 * block with no write in flight when it was issued, on every device
	 * that confirmed it.  The devices owed a readd keep theirs, which is
	 * what the names below are made of.  A written record of a region the
	 * last block does not list keeps everything: nothing names it here.
	 */
	for (u32 i = 0; snapshot && i < onr; i++) {
		const struct btrfs_wib_names *names = &wib->readd_names[i];
		struct btrfs_wib_written *w;
		struct btrfs_wib_entry old;
		u64 done;

		btrfs_wib_read_entry(wib->last, i, &old);
		w = wib_written_find(wib, old.bytenr);
		if (!w)
			continue;
		done = ~wib_snapbits(wib, old.bytenr);
		w->cols &= ~(done & ~names->stale);
		w->par &= ~(done & ~names->stale_par);
	}
	/*
	 * Plan before touching anything.  @nr_union is how many regions the
	 * block written after this readd lists, whatever it takes back:
	 * everything the set holds and everything the last block lists.
	 * @nr_perm is what is left of it once the writes in flight that the
	 * last block does not list are done -- those are marks whose record
	 * has not been written yet, so nothing of theirs is on a disk, and
	 * each either gets written with that record or fails without it.
	 * @forced_wide: the block is wide whatever happens here, because the
	 * set or the last block already carries a stale record.  @naming: a
	 * name falls on a block this readd would name.
	 */
	live = wib_live_count(wib);
	nr_union = live;
	forced_wide = wib_live_max(wib) == BTRFS_WIB_MAX_ENTRIES;
	bitmap_zero(in_last, BTRFS_WIB_NR_ENTRIES);
	for (u32 i = 0; i < onr; i++) {
		const struct btrfs_wib_entry *e;
		struct btrfs_wib_entry old;
		u64 back = 0;
		u64 inflight = 0;
		u64 settled = 0;
		u64 nstale, npar;

		btrfs_wib_read_entry(wib->last, i, &old);
		e = wib_find_entry(wib, old.bytenr);
		if (e) {
			__set_bit(e - wib->entries, in_last);
			back = wib_entry_bits(&old) & ~(e->bitmap | e->sticky);
			inflight = wib_entry_bits(&old) & e->bitmap;
			if (precise)
				settled = wib_entry_bits(&old) & e->sticky & ~e->bitmap;
		} else {
			nr_union++;
			back = wib_entry_bits(&old);
		}
		if (old.stale | old.stale_par)
			forced_wide = true;
		wib_readd_names(wib, i, old.bytenr, precise, &nstale, &npar);
		if ((nstale | npar) & (back | inflight | settled))
			naming = true;
	}
	nr_perm = onr;
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		const struct btrfs_wib_entry *e = &wib->entries[i];

		if (wib_entry_used(e) && e->sticky && !test_bit(i, in_last))
			nr_perm++;
	}
	cap = forced_wide ? BTRFS_WIB_MAX_ENTRIES : BTRFS_WIB_MAX_ENTRIES_V1;
	if (nr_union <= (naming ? BTRFS_WIB_MAX_ENTRIES : cap))
		plan = WIB_READD_NAMED;
	else if (nr_union <= cap)
		plan = WIB_READD_UNNAMED;
	else
		plan = nr_perm <= cap ? WIB_READD_WAIT : WIB_READD_LOSE;
	/*
	 * No write took more of the room since the readd was first owed
	 * (@readd_admit_last in struct btrfs_wib), so writes that hold it this
	 * long are stuck on a device, and every recorded write fails while
	 * they do.  Stop waiting for them.
	 */
	gave_up = plan == WIB_READD_WAIT && was_owed && !READ_ONCE(readd_admits_new) &&
		  time_after_eq(jiffies, wib->readd_until);
	if (gave_up)
		plan = WIB_READD_LOSE;
	/*
	 * Names are free when the block is wide anyway; otherwise only when
	 * everything fits in the wide layout.  NAMED guarantees every entry
	 * taken back finds room below @limit; LOSE fills up to it.
	 */
	name = plan == WIB_READD_NAMED || (plan == WIB_READD_LOSE && forced_wide);
	limit = name && naming ? BTRFS_WIB_MAX_ENTRIES : cap;

	for (u32 i = 0; plan != WIB_READD_WAIT && i < onr; i++) {
		struct btrfs_wib_entry old;
		struct btrfs_wib_entry *e;
		u64 back, inflight, settled, stale, stale_par, nstale, npar;

		btrfs_wib_read_entry(wib->last, i, &old);
		e = wib_find_entry(wib, old.bytenr);
		back = wib_entry_bits(&old) & ~(e ? e->bitmap | e->sticky : 0);
		inflight = e ? wib_entry_bits(&old) & e->bitmap : 0;
		settled = e && precise ? wib_entry_bits(&old) & e->sticky & ~e->bitmap : 0;
		if (!e && back) {
			if (live < limit)
				e = wib_find_or_alloc_entry(wib, old.bytenr, false);
			if (!e) {
				nr_lost += hweight64(back);
				continue;
			}
			live++;
		}
		if (!e)
			continue;
		wib_readd_names(wib, i, old.bytenr, precise, &nstale, &npar);
		nstale &= back | inflight | settled;
		npar &= back | inflight | settled;
		/*
		 * A write in flight is kept recorded where a name falls on it:
		 * a column or parity on a device that failed the flush, that a
		 * write went to (any, with raid56_wf_name_unwritten=1).
		 */
		if (!((nstale | npar) & inflight))
			inflight = 0;
		if (back | inflight) {
			e->sticky |= back | inflight;
			e->gen = max(e->gen, fs_info->generation);
			nr_readded += hweight64(back | inflight);
		}
		/*
		 * Carry the stale record back with it.  Re-adding these blocks
		 * as plain sticky would say "something went wrong here" while
		 * dropping "and it was the DATA that is wrong" -- and a scrub
		 * that sees only the first recomputes the parity from the
		 * stale sector, which is the exact loss this record exists to
		 * prevent.  A device failing to confirm a flush is no reason
		 * to forget which side of the stripe was bad.
		 *
		 * Then add the names.  Every block named is in @sticky by now,
		 * which keeps @stale a subset of it.  A member named needs its
		 * written record no more: the name says what it did, and a
		 * later write to it notes itself again.
		 */
		stale = back ? old.stale : 0;
		stale_par = back ? old.stale_par : 0;
		if (name) {
			struct btrfs_wib_written *w = wib_written_find(wib, old.bytenr);

			stale |= nstale;
			stale_par |= npar;
			e->hold |= nstale & inflight;
			e->hold_par |= npar & inflight;
			wib->readd_names[i].repair = (nstale | npar) &
						     ~wib->readd_names[i].absent;
			if (precise && w) {
				w->cols &= ~nstale;
				w->par &= ~npar;
			}
		} else {
			nr_unnamed += hweight64(nstale) + hweight64(npar);
			/*
			 * The write to that member may be lost, and the record
			 * cannot say so: whatever the flush took, the parity
			 * the other devices kept may not describe the stripe.
			 * Say at least that, so that a mount after a crash
			 * treats it as the write in flight it was, not as a
			 * failed write whose damage the record names.
			 */
			if (!READ_ONCE(all_records_torn))
				e->torn |= ((nstale & ~e->stale) | (npar & ~e->stale_par)) &
					   e->sticky;
		}
		/*
		 * The blocks whose write finished before the flush was issued
		 * (@snapshot has none of them in flight), taken back as error
		 * records: every member of theirs on a device that confirmed
		 * the flush is on stable media, and every member on one that
		 * did not and that a write went to is named above -- or marked
		 * possibly torn where it could not be, and that mark is written
		 * as in flight anyway.  A member no write went to holds what it
		 * held.  So nothing about such a write is in flight any more,
		 * and listing it so on disk says what the record does not: a
		 * write that may have been torn, which the next mount keeps
		 * undecided wherever its named columns take every parity to
		 * rebuild (SCRUB_WIB_TORN) -- although the parity describes the
		 * data exactly as the name says.  The block written after this
		 * lists them as the error records they are (wib_readd_base()).
		 * Not a block still in flight or in flight at @snapshot: that
		 * write may have landed during the flush, into a cache nobody
		 * vouches for.  Not a block the set marks possibly torn, nor
		 * one the set no longer holds that the last block lists as an
		 * error record as well as in flight: that is how a possibly
		 * torn mark is written, and a full log may have spent it
		 * (wib_entry_torn_only()).  Not without a snapshot, which
		 * cannot say.  And not when no device is known to have failed
		 * the flush (a barrier to a device gone from under it): nothing
		 * names what that device may have lost.
		 */
		if (snapshot && identified && !READ_ONCE(finished_stay_inflight))
			wib->readd_names[i].landed = old.bitmap &
						     ((back & ~old.sticky) | settled) &
						     ~wib_snapbits(wib, old.bytenr) &
						     e->sticky & ~(e->bitmap | e->torn);
		/*
		 * A name on a column a running replace had marked says the
		 * source may be stale there too, whether or not the bit is
		 * already set.  A mark carried back that the set still holds
		 * says nothing new: it is the one the last block was written
		 * from, and stays whose it was -- a running replace's, or the
		 * recovery's verdict (@suspect_par, @prior_par).  Taken over,
		 * those became plain stale records that a full log with a
		 * device missing spends in table order (wib_evict_sticky()),
		 * whenever a write that finished in the same region gave the
		 * readd anything to take back.
		 */
		if (READ_ONCE(readd_disowns_all))
			wib_replace_disown(e, stale, stale_par);
		else
			wib_replace_disown(e, (name ? nstale : 0) | (stale & ~e->stale),
					   (name ? npar : 0) | (stale_par & ~e->stale_par));
		stale &= e->sticky & ~e->stale;
		stale_par &= ~e->stale_par;
		if (!(stale | stale_par))
			continue;
		nr_named += hweight64(stale & nstale) + hweight64(stale_par & npar);
		e->stale |= stale;
		atomic_add(hweight64(stale), &wib->nr_stale);
		wib_set_stale_par(wib, e, e->stale_par | stale_par);
		e->gen = max(e->gen, fs_info->generation);
	}
	wib->readd_landed = plan != WIB_READD_WAIT;
	spin_unlock_irqrestore(&wib->lock, flags);

	if (nr_readded)
		atomic64_add(nr_readded, &wib->stat_sticky);
	/*
	 * Say so, as a write that failed on the device would have: the
	 * stripes have lost their redundancy until the repairs asked for here
	 * land.  Only for names this readd added; any others were announced
	 * when they were made.
	 */
	if (plan != WIB_READD_WAIT && !READ_ONCE(readd_no_repair)) {
		const u64 first = wib_readd_queue_repairs(wib, onr);

		if (nr_named)
			raid56_alert_failed(fs_info, BTRFS_RAID56_EV_STALE, first,
					    &wib->readd_owed_failed);
	}
	wib_readd_set_owed(wib, plan == WIB_READD_WAIT);
	if (!wib->readd_owed)
		memset(&wib->readd_owed_failed, 0, sizeof(wib->readd_owed_failed));

	/* Latched, both: someone has to look, and acknowledge. */
	if (nr_unnamed)
		btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_LOG_UNFLUSHED, 0, NULL, 0);
	if (nr_lost)
		btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_DROPPED, 0, NULL, 0);
	if (btrfs_is_testing(fs_info))
		return !wib->readd_owed;
	if (nr_named)
		btrfs_warn_rl(fs_info,
	"raid56 write-intent log: keeping %u stripes recorded whose data %s did not confirm is flushed, and marking its column or parity stale in %u",
			      nr_readded, who, nr_named);
	else if (nr_readded)
		btrfs_warn_rl(fs_info,
	"raid56 write-intent log: keeping %u stripes recorded, a device did not confirm their data is flushed",
			      nr_readded);
	if (nr_unnamed)
		btrfs_err_rl(fs_info,
	"raid56 write-intent log: %s did not confirm a flush, and the log is too full to say so in %u of the stripes it keeps recorded: data without checksums there may read back an older version",
			     who, nr_unnamed);
	if (gave_up)
		btrfs_err_rl(fs_info,
	"raid56 write-intent log: writes in flight held the room for the records a failed flush kept for %u s; not waiting for them any longer",
			     jiffies_to_msecs(BTRFS_WIB_READD_WAIT) / 1000);
	if (nr_lost)
		btrfs_err_rl(fs_info,
	"raid56 write-intent log full, %u stripes whose data %s did not confirm is flushed are not recorded any more, run scrub",
			     nr_lost, who);
	if (plan == WIB_READD_WAIT && !was_owed)
		wib_readd_say_wait(wib, who);
	else if (plan != WIB_READD_WAIT && was_owed && wib->readd_said)
		btrfs_info(fs_info,
	"raid56 write-intent log: took back the records a failed flush kept, dropping from the log again");
	return !wib->readd_owed;
}

/*
 * A transaction commit is between its snapshot and its barriers (or past
 * them), and a block is about to be written: add the current set to the
 * snapshot.  Uses wib->block as scratch.  commit_mutex must be held.
 *
 * A mark waits for the first block with a sequence number at least the one it
 * was promised (wib_commit_wait()), and that is any block: its own lazy
 * commit's, or one btrfs_wib_persist_now() or a flush-and-drop writes first.
 * Either way the mark was made before the block's number was taken, so its
 * bits are in the set now, and once the block is written its writer issues
 * its bios -- perhaps while that commit's barrier flushes the devices.  The
 * commit must then not drop the stripe on the strength of the barrier, even
 * if the write finishes before the commit's drop; nor may it take the
 * stripe's written records for flushed (wib_written_flushed()).  In its
 * snapshot as in flight, it does neither.
 */
static void wib_fold_prepared_locked(struct btrfs_wib *wib)
{
	lockdep_assert_held(&wib->commit_mutex);

	if (!wib->prepared_valid)
		return;
	if (btrfs_wib_build_block(wib, wib->block, 0, wib->prepared) == 0)
		memcpy(wib->prepared, wib->block, BTRFS_WIB_SLOT_SIZE);
	else
		wib->prepared_valid = false;
}

/*
 * Build wib->block from the in-memory set plus everything @base lists, and
 * write it.  -ENOSPC if that does not fit in a block (nothing is written).
 * commit_mutex must be held.
 */
static int wib_write_block_locked(struct btrfs_wib *wib, u64 seq, const void *base,
				  bool force)
{
	unsigned long flags;
	struct btrfs_wib_disk_header *hdr = wib->block;
	struct btrfs_wib_disk_header *last = wib->last;
	int nr_errors;
	int ret;

	lockdep_assert_held(&wib->commit_mutex);

	/* Whoever writes it, this block may satisfy a mark. */
	if (!READ_ONCE(snapshot_misses_marks))
		wib_fold_prepared_locked(wib);
	ret = btrfs_wib_build_block(wib, wib->block, seq, base);
	if (ret < 0)
		return ret;

	/*
	 * Nothing changed since the last commit and that one reached every
	 * device: the set is already durable, no IO needed.
	 */
	if (!force && wib->last_ok &&
	    le64_to_cpu(last->magic) == BTRFS_WIB_MAGIC &&
	    le32_to_cpu(last->nr_entries) == le32_to_cpu(hdr->nr_entries) &&
	    memcmp(wib->last + sizeof(*hdr), wib->block + sizeof(*hdr),
		   le32_to_cpu(hdr->nr_entries) * sizeof(struct btrfs_wib_disk_entry)) == 0)
		goto done;

	ret = wib_write_all_devices(wib, &nr_errors);
	if (nr_errors || ret < 0)
		atomic64_inc(&wib->stat_commit_errors);
	/*
	 * Whether or not the write succeeded, this is the block every device
	 * either has or has an older subset of; the next union is built on it.
	 */
	memcpy(wib->last, wib->block, BTRFS_WIB_SLOT_SIZE);
	wib->last_ok = (ret == 0 && nr_errors == 0);
	if (ret < 0)
		return ret;
	atomic64_inc(&wib->stat_commits);
done:
	spin_lock_irqsave(&wib->lock, flags);
	wib->seq = seq;
	spin_unlock_irqrestore(&wib->lock, flags);
	wake_up_all(&wib->wait);
	return 0;
}

/*
 * The base the block written after a readd unions: @last, with the blocks the
 * readd found landed (@landed in struct btrfs_wib_names) listed as the error
 * records it took them back as rather than in flight.  Nothing @last lists
 * stops being listed, so the block needs no flush, as a union with @last does
 * not; only what the next mount makes of those blocks changes -- the record of
 * a failed write whose damage the names say, not a write in flight at a crash
 * that may have torn the stripe (wib_pending_in_flight()).  @last itself stays
 * the block the devices hold.  commit_mutex held, right after
 * wib_readd_dropped().
 */
static const void *wib_readd_base(struct btrfs_wib *wib)
{
	const struct btrfs_wib_disk_header *oh = wib->last;
	bool any = false;
	u32 onr;

	lockdep_assert_held(&wib->commit_mutex);

	if (!wib->readd_landed || le64_to_cpu(oh->magic) != BTRFS_WIB_MAGIC)
		return wib->last;
	onr = min(le32_to_cpu(oh->nr_entries), wib_block_max_entries(wib->last));
	for (u32 i = 0; i < onr; i++) {
		const u64 landed = wib->readd_names[i].landed;
		struct btrfs_wib_entry e;

		if (!landed)
			continue;
		if (!any)
			memcpy(wib->readd_base, wib->last, BTRFS_WIB_SLOT_SIZE);
		any = true;
		btrfs_wib_read_entry(wib->readd_base, i, &e);
		e.sticky |= e.bitmap & landed;
		e.bitmap &= ~landed;
		wib_write_entry(wib->readd_base, i, &e);
	}
	return any ? wib->readd_base : wib->last;
}

/*
 * Write a block that drops the stripes that finished before @snapshot was
 * taken, the devices having been flushed after that (@flushed: every
 * device confirmed).  A stripe that finished after the snapshot may have
 * completed its writes during the flush, so it stays listed.
 *
 * If a device did not confirm the flush nothing is dropped: the block is
 * the union with the last one, and the stripes that would have been
 * dropped, and those with a write in flight, become error records naming
 * the devices in @failed (they may hold stale sectors).  The same while an
 * earlier failed flush is still owed a readd, whatever this one did -- see
 * wib_readd_dropped().
 *
 * @timed: the flush was issued after @snapshot was taken, so what it made
 * durable can be told from it (struct btrfs_wib_written).  Not so for a
 * snapshot taken without one before it (btrfs_wib_commit()).
 */
static int wib_drop_locked(struct btrfs_wib *wib, u64 seq, const void *snapshot,
			   bool flushed, const struct btrfs_wib_flush_failed *failed,
			   bool force, bool timed)
{
	unsigned long flags;
	int ret;

	lockdep_assert_held(&wib->commit_mutex);

	if (!flushed || wib->readd_owed) {
		wib_readd_dropped(wib, flushed ? NULL : failed, timed ? snapshot : NULL);
		return wib_write_block_locked(wib, seq, wib_readd_base(wib), force);
	}
	if (timed)
		wib_written_flushed(wib, snapshot);
	ret = wib_write_block_locked(wib, seq, snapshot, force);
	/*
	 * Nothing listed any more, so every write noted or not has been
	 * covered by a flush every device confirmed: what a full table of
	 * written records could not keep is known again.
	 */
	if (ret == 0 && timed &&
	    le32_to_cpu(((const struct btrfs_wib_disk_header *)wib->last)->nr_entries) == 0) {
		spin_lock_irqsave(&wib->lock, flags);
		wib->written_unknown = false;
		spin_unlock_irqrestore(&wib->lock, flags);
	}
	return ret;
}

/*
 * Flush every device and drop the stripes that finished before the flush.
 * Retried a few times if stripes turned over during the flush faster than
 * a block can hold.
 */
static int wib_flush_and_drop_locked(struct btrfs_wib *wib, u64 seq, bool force)
{
	int ret = -ENOSPC;

	lockdep_assert_held(&wib->commit_mutex);

	for (int i = 0; i < 3 && ret == -ENOSPC; i++) {
		int flush;

		/*
		 * Build into wib->flushsnap.  Not wib->prepared: that holds the
		 * snapshot a transaction commit took before its barriers, and
		 * the commit drops against it on the strength of those
		 * barriers, so replacing it here with a snapshot taken after
		 * them would let that commit drop a stripe no flush covered.
		 * And not wib->block either: wib_drop_locked() hands the
		 * snapshot to wib_write_block_locked(), which builds into
		 * wib->block and memsets it first -- the snapshot would be
		 * zeroed before it was read, silently contributing nothing,
		 * and the block written would then omit every stripe that
		 * finished during the flush.
		 */
		ret = btrfs_wib_build_block(wib, wib->flushsnap, seq, NULL);
		/*
		 * The in-memory set always fits: wib_live_max() caps it at what
		 * the current layout can describe, and wib_enforce_capacity_locked()
		 * re-establishes that whenever a stale bit halves the cap.
		 *
		 * Check it anyway rather than only asserting.  ASSERT() compiles
		 * away without CONFIG_BTRFS_ASSERT, and the block handed on from
		 * here is the snapshot of what was in flight: a failed build
		 * leaves it zeroed with no magic, which reads as "nothing was in
		 * flight" and drops stripes that no flush covered.  Refusing the
		 * commit costs a failed write; continuing costs the write hole.
		 */
		ASSERT(ret == 0);
		if (ret)
			return ret;
		flush = wib_flush_all_devices(wib, &wib->flush_failed);
		ret = wib_drop_locked(wib, seq, wib->flushsnap, flush == 0,
				      &wib->flush_failed, force, flush >= 0);
	}
	return ret;
}

/*
 * Take a snapshot of the in-flight set and persist it, adding to the last
 * block (which needs no flush) if that fits, else flushing and dropping.
 * commit_mutex must be held.
 */
static int wib_commit_locked(struct btrfs_wib *wib, bool force)
{
	unsigned long flags;
	u64 seq;
	int ret;

	lockdep_assert_held(&wib->commit_mutex);

	spin_lock_irqsave(&wib->lock, flags);
	seq = ++wib->snap_seq;
	spin_unlock_irqrestore(&wib->lock, flags);

	/*
	 * A transaction commit is between its snapshot and its barriers (or
	 * past them): a stripe recorded now may write during or after the
	 * flush, so it must not be dropped by that commit even if it finishes
	 * before then.  wib_write_block_locked() adds the current set to the
	 * snapshot; with raid56_wf_snapshot_misses_marks=1 only this does.
	 */
	if (READ_ONCE(snapshot_misses_marks))
		wib_fold_prepared_locked(wib);

	ret = wib_write_block_locked(wib, seq, wib->last, force);
	if (ret == -ENOSPC)
		ret = wib_flush_and_drop_locked(wib, seq, force);
	return ret;
}

/*
 * Wait until a commit with sequence number >= @want has completed.  The
 * commits issued here only add to the on-disk block, so they need no
 * flush.
 */
static int wib_commit_wait(struct btrfs_wib *wib, u64 want)
{
	unsigned long flags;
	int ret = 0;

	mutex_lock(&wib->commit_mutex);
	while (true) {
		u64 seq;

		spin_lock_irqsave(&wib->lock, flags);
		seq = wib->seq;
		spin_unlock_irqrestore(&wib->lock, flags);
		if (seq >= want)
			break;
		ret = wib_commit_locked(wib, false);
		if (ret < 0)
			break;
	}
	mutex_unlock(&wib->commit_mutex);
	return ret;
}

/*
 * A write into a region the last block does not list and the set holds no
 * record of, while a failed flush's readd is owed: see @readd_admit_last in
 * struct btrfs_wib.
 * Plan the readd again -- once the writes that held its room are gone, nothing
 * else may, the next transaction commit being as far away as it likes -- and
 * while it is still owed, wait for them to go, or for BTRFS_WIB_READD_WAIT to
 * run out and the next plan to stop waiting.  False once @deadline has passed.
 */
static bool wib_readd_settle(struct btrfs_wib *wib, unsigned long deadline)
{
	unsigned long flags;
	unsigned long until;
	long timeout;
	bool owed;

	mutex_lock(&wib->commit_mutex);
	if (wib->readd_owed)
		wib_readd_dropped(wib, NULL, NULL);
	owed = wib->readd_owed;
	mutex_unlock(&wib->commit_mutex);
	if (!owed)
		return true;
	if (time_after_eq(jiffies, deadline))
		return false;

	spin_lock_irqsave(&wib->lock, flags);
	until = wib->readd_until;
	spin_unlock_irqrestore(&wib->lock, flags);
	if (time_before(until, deadline))
		deadline = until;
	timeout = max_t(long, (long)(deadline - jiffies), 1);
	wait_event_timeout(wib->wait, !wib_readd_waiting(wib), timeout);
	return true;
}

/*
 * Record that a sub-stripe (or in-place) write of the full stripe covering
 * [@logical, @logical + @len) is about to be submitted.
 *
 * Returns only after the record is durable on the devices (or immediately
 * if the log is not enabled).  Must not be called with locks held that the
 * commit path or device IO completion could depend on.  On failure nothing
 * stays recorded and the caller must not write.
 */
int btrfs_wib_mark(struct btrfs_fs_info *fs_info, u64 logical, u64 len)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	unsigned long deadline = 0;
	enum wib_room room;
	unsigned long timeout = 0;
	bool spend_torn = READ_ONCE(torn_spent_eagerly);
	bool last = false;
	bool enabled;
	bool owed;
	u64 want;
	int ret;

	if (!wib)
		return 0;
	ASSERT(len > 0);

	/* Full stripes are 64K aligned; be safe against any other caller. */
	len = round_up(logical + len, BTRFS_WIB_BLOCK_SIZE);
	logical = round_down(logical, BTRFS_WIB_BLOCK_SIZE);
	len -= logical;

	while (true) {
		spin_lock_irqsave(&wib->lock, flags);
		owed = wib_readd_refuses_locked(wib, logical, len);
		ret = owed ? -ENOSPC : wib_try_mark_locked(wib, logical, len, spend_torn);
		if (ret) {
			/*
			 * Sampled in the same locked section as the failure: a
			 * write finishing in between would otherwise look like
			 * nothing left to wait for, and fail a write that now
			 * fits.
			 */
			room = wib_room_source_locked(wib);
			/*
			 * Nothing else can make room: spend a record that only
			 * says a write may have been torn, if there is one
			 * (wib_entry_torn_only()).
			 */
			if (!owed && !spend_torn && !wib_room_without_torn_locked(wib)) {
				spend_torn = true;
				ret = wib_try_mark_locked(wib, logical, len, true);
			}
		}
		if (ret == 0) {
			/*
			 * The next snapshot is guaranteed to contain our
			 * bits, wait for the commit that writes it.
			 */
			want = wib->snap_seq + 1;
			enabled = wib->enabled;
			spin_unlock_irqrestore(&wib->lock, flags);
			break;
		}
		spin_unlock_irqrestore(&wib->lock, flags);

		/* Not full: the room is the readd's, see wib_readd_settle(). */
		if (owed) {
			if (!deadline)
				deadline = jiffies + BTRFS_WIB_FULL_TIMEOUT;
			if (wib_readd_settle(wib, deadline))
				continue;
			btrfs_err_rl(fs_info,
	"raid56 write-intent log: a failed flush's records still wait for room after %u ms, failing the write",
				     jiffies_to_msecs(BTRFS_WIB_FULL_TIMEOUT));
			btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_LOG_WRITE, logical, NULL, 0);
			return -EIO;
		}

		/*
		 * Log full.  Entries are freed when in-flight RMWs finish.
		 * Those RMWs can themselves be waiting on commit_mutex and on
		 * IO to devices this one knows nothing about, so this is not
		 * a wait that is guaranteed to end: bound it and fail the
		 * write rather than hang the task forever.
		 */
		/*
		 * Only a write in flight or a repair can make room.  With
		 * neither, the log is full of records that must stay -- they
		 * name stale data on a device that keeps failing -- and waiting
		 * would only make every write take a minute to fail.  Fail it
		 * now, and wait below only while something can still finish.
		 */
		if (room == WIB_ROOM_NONE) {
			btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_LOG_FULL, logical, NULL, 0);
			return -EIO;
		}
		if (last) {
			btrfs_err_rl(fs_info,
	"raid56 write-intent log still full after %u ms, failing the write",
				     jiffies_to_msecs(timeout));
			btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_LOG_FULL, logical, NULL, 0);
			return -EIO;
		}
		/* A write in flight will finish; a repair may never land. */
		timeout = room == WIB_ROOM_WRITE ? BTRFS_WIB_FULL_TIMEOUT :
			  msecs_to_jiffies(READ_ONCE(log_full_repair_wait_ms));
		btrfs_warn_rl(fs_info,
			      "raid56 write-intent log full, waiting for in-flight writes and repairs");
		/*
		 * Waited, and still full: once more, spending a record that
		 * only says a write may have been torn if there is one, before
		 * failing the write.
		 */
		if (!wait_event_timeout(wib->wait,
					wib_can_mark(wib, logical, len, spend_torn) ||
					!wib_can_make_room(wib, spend_torn),
					timeout)) {
			spend_torn = true;
			last = true;
		}
	}
	atomic64_inc(&wib->stat_marks);

	if (!enabled)
		return 0;
	ret = wib_commit_wait(wib, want);
	if (ret < 0) {
		/* Nothing will be written, don't leave the bits in flight. */
		btrfs_wib_done(fs_info, logical, len, false);
		/*
		 * The write fails without a single byte reaching a disk, and
		 * nothing else says why: every recorded write will, until the
		 * cause goes.
		 */
		btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_LOG_WRITE, logical, NULL, 0);
	}
	return ret;
}

/*
 * All writes of the RMW recorded by btrfs_wib_mark() have completed.
 *
 * @failed: at least one of them failed (device error or missing device).
 * The stripe is then inconsistent on that device without any crash; keep it
 * logged so that it is scrubbed once the device is back, replaced or
 * dropped, at the next mount.
 */
void btrfs_wib_done(struct btrfs_fs_info *fs_info, u64 logical, u64 len, bool failed)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	u64 end;
	bool freed = false;

	if (!wib)
		return;

	end = round_up(logical + len, BTRFS_WIB_BLOCK_SIZE);
	logical = round_down(logical, BTRFS_WIB_BLOCK_SIZE);
	len = end - logical;

	spin_lock_irqsave(&wib->lock, flags);
	for (u64 cur = wib_entry_bytenr(logical); cur < end; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_entry *e = wib_find_entry(wib, cur);
		u64 mask;

		if (!e)
			continue;
		mask = btrfs_wib_range_mask(cur, logical, len);
		e->bitmap &= ~mask;
		if (failed) {
			e->sticky |= mask;
			e->gen = max(e->gen, fs_info->generation);
			atomic64_inc(&wib->stat_sticky);
		}
		/*
		 * Deliberately NOT clearing e->stale here.  @len is the whole
		 * full stripe, but an RMW writes only the data stripes it was
		 * given -- so clearing across the range would turn the alarm
		 * off for columns this write never touched and which are still
		 * stale on disk.  rmw_update_stale_data() clears exactly the
		 * columns whose writes landed.
		 */
		if (!e->bitmap)
			freed = true;
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	if (freed)
		wake_up_all(&wib->wait);
}

/*
 * Keep [@logical, @logical + @len) recorded across mounts without a write in
 * flight: used for stripes whose recovery could not complete.  Dropped with
 * a warning if the log is full.
 */
/*
 * Record [@logical, @logical + @len) as having had a failed write.  Returns
 * -ENOSPC, recording nothing and saying nothing, if the log has no room: for
 * a caller that can still fail the write instead.
 */
int btrfs_wib_try_add_sticky(struct btrfs_fs_info *fs_info, u64 logical, u64 len)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	int ret;

	if (!wib)
		return 0;

	spin_lock_irqsave(&wib->lock, flags);
	ret = btrfs_wib_try_mark(wib, logical, len);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret < 0)
		return ret;
	btrfs_wib_done(fs_info, logical, len, true);
	return 0;
}

void btrfs_wib_add_sticky(struct btrfs_fs_info *fs_info, u64 logical, u64 len)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	int ret;

	if (!wib)
		return;

	spin_lock_irqsave(&wib->lock, flags);
	ret = btrfs_wib_try_mark(wib, logical, len);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret < 0) {
		btrfs_warn(fs_info,
	"raid56 write-intent log full, cannot keep full stripe at %llu for the next mount, run scrub once all devices are present",
			   logical);
		btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_DROPPED, logical, NULL, 0);
		return;
	}
	btrfs_wib_done(fs_info, logical, len, true);
}

/* [@logical, @logical + @len) was fully recovered, forget its error record. */
/*
 * Record that the data of [@logical, @logical + @len) is stale on disk: a
 * write of it was acknowledged but the device did not take it, so what was
 * acknowledged survives only in the parity.
 *
 * The read path must treat these blocks the way it treats a sector that fails
 * its checksum -- reconstruct rather than believe.  For nodatacow data there
 * is no checksum to fail, so without this the next read-modify-write of the
 * same full stripe reads the stale sector, trusts it, and computes a parity
 * from it, which destroys the only remaining copy of the acknowledged value.
 */
void btrfs_wib_mark_stale(struct btrfs_fs_info *fs_info, u64 logical, u64 len)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	u64 end;

	if (!wib)
		return;

	end = round_up(logical + len, BTRFS_WIB_BLOCK_SIZE);
	logical = round_down(logical, BTRFS_WIB_BLOCK_SIZE);
	len = end - logical;

	spin_lock_irqsave(&wib->lock, flags);
	for (u64 cur = wib_entry_bytenr(logical); cur < end; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_entry *e = wib_find_entry(wib, cur);
		u64 mask, add;

		if (!e)
			continue;
		mask = btrfs_wib_range_mask(cur, logical, len);
		/*
		 * Only where the stripe is already recorded.  A block that is
		 * not sticky has nothing carrying its value, so calling it
		 * stale would send the read path to a parity that describes
		 * exactly the sector it is being told to distrust.
		 */
		add = (mask & e->sticky) & ~e->stale;
		wib_replace_disown(e, mask & e->sticky, 0);
		e->stale |= mask & e->sticky;
		if (add)
			atomic_add(hweight64(add), &wib->nr_stale);
	}
	/* The first stale bit halves the capacity; make the set fit it. */
	wib_enforce_capacity_locked(wib);
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * Is the data of the block containing @logical known to be stale on disk?
 *
 * Answers the question a checksum answers, for data that has none.  A false
 * negative is the behaviour without this record at all; a false positive
 * costs a reconstruction that was not needed.
 */
/* Is anything at all recorded stale?  Lock-free; see wib->nr_stale. */
bool btrfs_wib_any_stale(const struct btrfs_fs_info *fs_info)
{
	return fs_info->wib && atomic_read(&fs_info->wib->nr_stale) != 0;
}

/*
 * Will a record made now still be there after the next mount?
 *
 * Every record lives in memory first and reaches the disk with the next log
 * commit -- but only while the log is being persisted.  With the log off
 * (noraid56_write_intent, or an administrator's disable) a record protects the
 * reads of this mount and is gone at the next one.  For most callers that is
 * the best there is.  A caller about to put content on a disk that is only
 * safe to leave there BECAUSE it is recorded -- zeros standing in for sectors
 * a device replace could not reproduce, see scrub_replace_record_lost() --
 * has to know the difference: after an unmount the zeros would read back as
 * data.
 *
 * An enable that is requested counts, since it is carried out before the
 * commit's superblock is written; a requested disable does not.
 */
bool btrfs_wib_persisting(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	unsigned long flags;
	bool ret;

	if (!wib)
		return false;
	spin_lock_irqsave(&wib->lock, flags);
	ret = (wib->enabled || wib->enable_requested || wib->enable_in_progress) &&
	      !wib->disable_requested;
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/*
 * A device replace is about to write zeros to its target for the sectors of the
 * data column at @logical that it could neither copy nor rebuild, and records
 * the column stale first (scrub_replace_record_lost()); the full stripe must be
 * recorded already (btrfs_wib_try_add_sticky()).  Returns whether the column is
 * recorded stale afterwards: marking can make the log shrink to fit, and a
 * record that did not survive that is no record.
 *
 * The log records whole columns, so the mark is not only about the zeros:
 * every sector of the column without a checksum is read through a rebuild
 * from then on, and fails wherever the rebuild cannot be done, although the
 * target holds the right bytes there.  That is the price of the zeros never
 * being read as data.  But none of it is true of the SOURCE, which is what
 * every read of the column is served from until the replace finishes -- and a
 * replace that is cancelled or fails leaves the source in the filesystem and
 * the target gone.  Marked as btrfs_wib_mark_stale() would, the source's
 * column was distrusted from the moment of the copy on: its good sectors
 * without a checksum failed to read wherever a sibling was unreadable too,
 * rebuilds of its siblings refused to use it, and after an aborted replace a
 * scrub or the next write rebuilt the whole column from the parity and wrote
 * that over it, and a replace tried again rebuilt instead of copying it.
 *
 * With @owned, a mark this adds is the replace's own until it ends: left out
 * of what the log answers while the source serves the column, persisted all
 * the same (a crash leaves it on disk, and ordinary: see @replace_stale in
 * struct btrfs_wib_entry), handed over when the replace finishes and dropped
 * when it does not (btrfs_wib_replace_end()).  A column that was stale already
 * is not the replace's to hide or drop, and stays as it was -- but a full log
 * spends the record as the replace's all the same (@replace_keep): the zeros
 * are on the target either way.
 */
bool btrfs_wib_replace_mark_stale(struct btrfs_fs_info *fs_info, u64 logical,
				  bool owned)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 block = round_down(logical, BTRFS_WIB_BLOCK_SIZE);
	const u64 cur = wib_entry_bytenr(block);
	const u64 mask = btrfs_wib_range_mask(cur, block, BTRFS_WIB_BLOCK_SIZE);
	struct btrfs_wib_entry *e;
	unsigned long flags;
	bool ret;

	if (!wib)
		return false;

	spin_lock_irqsave(&wib->lock, flags);
	e = wib_find_entry(wib, cur);
	/* Only where the stripe is recorded, as btrfs_wib_mark_stale(). */
	if (e && (e->sticky & mask)) {
		const u64 add = mask & ~e->stale;

		if (add) {
			e->stale |= add;
			atomic_add(hweight64(add), &wib->nr_stale);
		}
		if (owned) {
			e->replace_stale |= add;
			if (!READ_ONCE(replace_keeps_added_only))
				e->replace_keep |= mask;
		} else {
			wib_replace_disown(e, mask, 0);
		}
	}
	wib_enforce_capacity_locked(wib);
	e = wib_find_entry(wib, cur);
	ret = e && (e->stale & mask);
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/*
 * btrfs_wib_replace_mark_stale() for parity @parity of the full stripe at
 * @full_stripe_start, which a replace could not copy to its target
 * (scrub_replace_copy_parity()): recorded as not describing the data, as
 * btrfs_wib_update_stale_parity() would, and owned the same way.
 */
bool btrfs_wib_replace_mark_parity(struct btrfs_fs_info *fs_info,
				   u64 full_stripe_start, int parity, bool owned)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 logical = full_stripe_start +
			    ((u64)parity << BTRFS_WIB_BLOCK_SHIFT);
	const u64 cur = wib_entry_bytenr(logical);
	const u64 mask = btrfs_wib_range_mask(cur, logical, BTRFS_WIB_BLOCK_SIZE);
	struct btrfs_wib_entry *e;
	unsigned long flags;
	bool ret;

	if (!wib)
		return false;

	spin_lock_irqsave(&wib->lock, flags);
	e = wib_find_entry(wib, cur);
	if (e) {
		const u64 add = mask & ~e->stale_par;

		wib_set_stale_par(wib, e, e->stale_par | mask);
		if (owned) {
			e->replace_stale_par |= add;
			if (!READ_ONCE(replace_keeps_added_only))
				e->replace_keep_par |= mask;
		} else {
			wib_replace_disown(e, 0, mask);
		}
	}
	wib_enforce_capacity_locked(wib);
	e = wib_find_entry(wib, cur);
	ret = e && (e->stale_par & mask);
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/*
 * The running device replace has ended.  @finished: its target is now the
 * member the source was.  Otherwise the target is gone and the source stays.
 *
 * Finished, the marks it made for the zeros on its target become ordinary
 * ones, from before the first read the target serves (the caller holds
 * dev_replace->rwsem, which every mapping of a read waits for).  Not
 * finished, they described only a device that is no longer part of anything,
 * and are dropped: the source keeps serving those columns as it did before the
 * replace, and a sector it could not return is a plain read error that the
 * next read or scrub meets again.  The full stripes stay recorded, with
 * nothing named, which the next scrub settles -- the replace found sectors
 * there it could not read, and that is worth one.  In memory now, on disk with
 * the next commit of the log; a crash before then leaves the marks in place,
 * which costs reads of those columns a rebuild and nothing worse.
 *
 * Marks the replace made in an earlier mount, before a crash or unmount it
 * then resumed from, were loaded as ordinary ones and are not dropped here;
 * the resumed replace made them its own again, copying from the start
 * (btrfs_wib_replace_resume_rewinds()).
 *
 * Nothing else changes hands.  The recovery's verdicts (@suspect_par) are no
 * mark of the replace's -- one it made for a parity is its own, not a verdict
 * (btrfs_wib_replace_mark_parity()) -- and releasing them with its own made
 * them ordinary stale parities: every block wide at once, a log holding more
 * of them than a wide block describes unwritable with nothing to make it fit
 * (btrfs_wib_build_block() failing in every commit), and a full log with a
 * device missing spending them in table order, first.  The capacity is
 * enforced all the same, in case anything else left the set over it.
 *
 * Returns 0, or -EIO without handing anything over if a full log had to spend
 * one of the replace's marks (btrfs_wib_replace_marks_lost()): the target
 * then holds zeros nothing records, and must not take over.  The caller fails
 * the replace, which drops the rest with @finished false.
 */
int btrfs_wib_replace_end(struct btrfs_fs_info *fs_info, bool finished)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool clears = READ_ONCE(replace_end_clears_verdicts);
	unsigned long flags;
	unsigned int nr = 0;

	if (!wib)
		return 0;

	spin_lock_irqsave(&wib->lock, flags);
	if (finished && wib->replace_marks_lost) {
		spin_unlock_irqrestore(&wib->lock, flags);
		if (!btrfs_is_testing(fs_info))
			btrfs_err(fs_info,
"raid56: device replace FAILED: the write-intent log, full with a device missing, had to drop its record of zeros the replace put on the new device where it could neither copy nor rebuild the old one, so the new device is not used; see the replace_record_dropped alert");
		return -EIO;
	}
	WRITE_ONCE(wib->replace_marks_lost, false);
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		struct btrfs_wib_entry *e = &wib->entries[i];
		const u64 stale = e->stale & e->replace_stale;
		const u64 stale_par = e->stale_par & e->replace_stale_par;

		if (clears) {
			wib_replace_disown(e, U64_MAX, U64_MAX);
		} else {
			e->replace_stale = 0;
			e->replace_stale_par = 0;
		}
		e->replace_keep = 0;
		e->replace_keep_par = 0;
		if (finished || !wib_entry_used(e) || !(stale | stale_par))
			continue;
		nr += hweight64(stale) + hweight64(stale_par);
		if (stale) {
			atomic_sub(hweight64(stale), &wib->nr_stale);
			e->stale &= ~stale;
		}
		wib_set_stale_par(wib, e, e->stale_par & ~stale_par);
	}
	if (!clears)
		wib_enforce_capacity_locked(wib);
	spin_unlock_irqrestore(&wib->lock, flags);

	if (!nr || btrfs_is_testing(fs_info))
		return 0;
	btrfs_warn(fs_info,
"raid56: device replace did not finish: dropped the %u stale mark(s) it had made for sectors it could neither copy nor rebuild onto its target, which is gone; the old device serves those columns as before, and its unreadable sectors still fail to read",
		   nr);
	if (READ_ONCE(wib->health) != BTRFS_RAID56_HEALTH_OK)
		raid56_alert_kick(wib, 0);
	return 0;
}

/*
 * Has a full log spent a record of the zeros the running device replace put
 * on its target (wib_evict_sticky())?  Then the replace fails: its copy stops
 * at the next stripe (should_cancel_scrub()), and it does not finish
 * (btrfs_wib_replace_end()), which clears this.  Lock-free: a stale answer
 * only lets the copy run a stripe further; the finish decides under the lock.
 */
bool btrfs_wib_replace_marks_lost(struct btrfs_fs_info *fs_info)
{
	return fs_info->wib && READ_ONCE(fs_info->wib->replace_marks_lost);
}

/*
 * A device replace is resuming after a crash or an unmount, and the part it
 * copied before met @nr_uncopyable sectors it could neither copy nor rebuild
 * (the count its item keeps).  Must it copy again from the start?
 *
 * Each of those sectors left zeros on the target and a stale mark saying so.
 * Only the mount that made a mark knows it for the replace's own
 * (@replace_keep in struct btrfs_wib_entry); this one loaded it as an ordinary
 * record naming a member, and a full log with a device missing spends those in
 * table order, with nothing to say the zeros are no longer recorded: the
 * replace, resumed past the stripe, would finish and the target serve them as
 * data.  Copied again, every such stripe is copied or recorded again, as the
 * replace's own -- as when a full log spends one of its marks
 * (btrfs_run_dev_replace()).
 */
bool btrfs_wib_replace_resume_rewinds(struct btrfs_fs_info *fs_info, u64 nr_uncopyable)
{
	return nr_uncopyable && fs_info->wib && !READ_ONCE(replace_keeps_added_only);
}

/*
 * The data of [@logical, @logical + @len) has been written and landed, so it
 * is no longer stale.  Separate from btrfs_wib_done() because that is told
 * about the whole full stripe while only some of its columns were written.
 *
 * @durable: it was written with FUA, so it is on the media, not merely in a
 * cache.  Without that, a mark a failed flush put on the column while this
 * write was in flight stays (see @hold in struct btrfs_wib_entry): part of
 * the write may have landed before the flush failed, and gone with the
 * cache.
 */
void btrfs_wib_clear_stale(struct btrfs_fs_info *fs_info, u64 logical, u64 len,
			   bool durable)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 end = logical + len;

	if (!wib || !atomic_read(&wib->nr_stale))
		return;

	spin_lock_irqsave(&wib->lock, flags);
	for (u64 cur = wib_entry_bytenr(logical); cur < end; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_entry *e = wib_find_entry(wib, cur);
		u64 gone;

		if (!e)
			continue;
		gone = e->stale & btrfs_wib_range_mask(cur, logical, len);
		if (durable)
			e->hold &= ~gone;
		else
			gone &= ~e->hold;
		wib_replace_disown(e, gone, 0);
		/* The column is rewritten, on the replace's target too. */
		e->replace_keep &= ~gone;
		if (gone) {
			atomic_sub(hweight64(gone), &wib->nr_stale);
			e->stale &= ~gone;
		}
	}
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * Record whether parity @parity of the full stripe at @full_stripe_start
 * describes the data on disk.  Called for every parity of every logged
 * read-modify-write, so that a parity whose write landed clears a record an
 * earlier failure left behind.
 */
void btrfs_wib_update_stale_parity(struct btrfs_fs_info *fs_info,
				   u64 full_stripe_start, int parity, bool stale)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 logical = full_stripe_start +
			    ((u64)parity << BTRFS_WIB_BLOCK_SHIFT);
	const u64 cur = wib_entry_bytenr(logical);
	struct btrfs_wib_entry *e;
	u64 mask;

	if (!wib)
		return;

	spin_lock_irqsave(&wib->lock, flags);
	e = wib_find_entry(wib, cur);
	if (e) {
		mask = btrfs_wib_range_mask(cur, logical, BTRFS_WIB_BLOCK_SIZE);
		wib_replace_disown(e, 0, mask);
		/*
		 * The parity went to the device's cache, not with FUA: a mark
		 * a failed flush put on it while this write was in flight
		 * stays (@hold_par).
		 */
		if (stale)
			wib_set_stale_par(wib, e, e->stale_par | mask);
		else
			wib_set_stale_par(wib, e, e->stale_par & ~(mask & ~e->hold_par));
	}
	if (stale)
		wib_enforce_capacity_locked(wib);
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * The mount's recovery could not decide the full stripe at @full_stripe_start
 * (@len of data): a write into it may have been torn, a device holding one of
 * its data columns is missing, and nothing is left to tell the column's
 * unchecksummed data from a rebuild out of a torn parity
 * (scrub_raid56_mark_suspect()).  Record parity @parity stale, as
 * btrfs_wib_update_stale_parity() does, as that verdict (@suspect_par in
 * struct btrfs_wib_entry), and the stripe possibly torn, which is what the
 * next mount reaches it again from -- wib_keep_torn() does the same once the
 * recovery returns, but the capacity is decided here.
 *
 * A bit something else already recorded stale stays that writer's.  Caller
 * has recorded the stripe (btrfs_wib_add_sticky()).
 */
void btrfs_wib_mark_suspect_parity(struct btrfs_fs_info *fs_info,
				   u64 full_stripe_start, u64 len, int parity)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 logical = full_stripe_start +
			    ((u64)parity << BTRFS_WIB_BLOCK_SHIFT);
	const u64 cur = wib_entry_bytenr(logical);
	struct btrfs_wib_entry *e;

	if (!wib)
		return;

	spin_lock_irqsave(&wib->lock, flags);
	/* Nothing is marked under raid56_wf_all_records_torn=1: see wib_keep_torn(). */
	if (!READ_ONCE(all_records_torn)) {
		for (u64 c = wib_entry_bytenr(full_stripe_start);
		     c < full_stripe_start + len; c += BTRFS_WIB_ENTRY_SIZE) {
			e = wib_find_entry(wib, c);
			if (e)
				e->torn |= btrfs_wib_range_mask(c, full_stripe_start, len) &
					   e->sticky;
		}
	}
	e = wib_find_entry(wib, cur);
	if (e) {
		const u64 mask = btrfs_wib_range_mask(cur, logical, BTRFS_WIB_BLOCK_SIZE);
		const u64 other = e->stale_par & ~e->suspect_par;

		wib_replace_disown(e, 0, mask);
		wib_set_stale_par(wib, e, e->stale_par | mask);
		e->suspect_par |= mask & ~other;
	}
	/* Only where the verdict does count as stale (wib_entry_wide()). */
	wib_enforce_capacity_locked(wib);
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * The recovery found parities @parities (bit p for parity p) of the full
 * stripe at @full_stripe_start recorded stale on a possibly torn stripe with a
 * data column on a missing device, leaving it more unknown columns than usable
 * parities (btrfs_scrub_raid56_full_stripe()): what may be an earlier mount's
 * verdict, reloaded from a wide block.  Have a full log spend them last, as a
 * verdict (@prior_par in struct btrfs_wib_entry).  Only bits recorded stale.
 *
 * Without this the verdict lasted only as long as the mount that reached it,
 * or as a narrow block's torn mark: reloaded from a wide one it was an
 * ordinary stale parity, taken in table order -- and the recovery keeps such
 * stripes first, at the lowest slots -- so the first degraded writes into a
 * full log spent it, and reads of the missing column's unchecksummed data
 * were rebuilt from the torn parity.
 */
void btrfs_wib_keep_prior_verdict(struct btrfs_fs_info *fs_info,
				  u64 full_stripe_start, u32 parities)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;

	if (!wib || READ_ONCE(reload_verdicts_plain))
		return;

	spin_lock_irqsave(&wib->lock, flags);
	for (int p = 0; p < 2; p++) {
		const u64 logical = full_stripe_start + ((u64)p << BTRFS_WIB_BLOCK_SHIFT);
		const u64 cur = wib_entry_bytenr(logical);
		struct btrfs_wib_entry *e;

		if (!(parities & BIT(p)))
			continue;
		e = wib_find_entry(wib, cur);
		if (e)
			e->prior_par |= btrfs_wib_range_mask(cur, logical, BTRFS_WIB_BLOCK_SIZE) &
					e->stale_par;
	}
	spin_unlock_irqrestore(&wib->lock, flags);
}

static int wib_pending_cmp(const void *a, const void *b);

/*
 * The pending entry for @bytenr, if it still answers for the block @mask
 * names: not taken over by the recovery (see @pending_taken in struct
 * btrfs_wib).
 */
static const struct btrfs_wib_entry *wib_pending_block(struct btrfs_wib *wib,
						       u64 bytenr, u64 mask)
{
	const struct btrfs_wib_entry key = { .bytenr = bytenr };
	const struct btrfs_wib_entry *pe;

	lockdep_assert_held(&wib->lock);
	pe = bsearch(&key, wib->pending, wib->nr_pending, sizeof(key),
		     wib_pending_cmp);
	if (pe && wib->pending_taken && (wib->pending_taken[pe - wib->pending] & mask))
		return NULL;
	return pe;
}

/*
 * The pending entry for @bytenr while it is being consulted, see
 * consult_pending, and for as long as the recovery has not taken the block
 * @mask names over.
 */
static const struct btrfs_wib_entry *wib_find_pending(struct btrfs_wib *wib, u64 bytenr,
						      u64 mask)
{
	lockdep_assert_held(&wib->lock);
	if (!wib->consult_pending)
		return NULL;
	return wib_pending_block(wib, bytenr, mask);
}

/*
 * The recovery takes the full stripe [@start, @start + @len) over from
 * @pending (@take), just before it recovers it: from here the live table
 * answers for it, and the queries that answer from @pending leave it out.
 * With @take false it gives the stripe back, when its recovery did not
 * happen after all.  Its stale marks leave (or rejoin) @nr_pending_stale with
 * it.
 */
EXPORT_FOR_TESTS
void btrfs_wib_take_pending(struct btrfs_wib *wib, u64 start, u64 len, bool take)
{
	unsigned long flags;

	spin_lock_irqsave(&wib->lock, flags);
	for (u64 cur = wib_entry_bytenr(start); wib->pending_taken && cur < start + len;
	     cur += BTRFS_WIB_ENTRY_SIZE) {
		const struct btrfs_wib_entry key = { .bytenr = cur };
		const struct btrfs_wib_entry *pe;
		u64 *taken;
		u64 change;
		unsigned int nr;

		pe = bsearch(&key, wib->pending, wib->nr_pending, sizeof(key),
			     wib_pending_cmp);
		if (!pe)
			continue;
		taken = &wib->pending_taken[pe - wib->pending];
		change = btrfs_wib_range_mask(cur, start, len) & (take ? ~*taken : *taken);
		*taken ^= change;
		if (!wib->consult_pending)
			continue;
		/* Counted as btrfs_wib_load() counted them. */
		nr = hweight64(pe->stale & change) + hweight64(pe->stale_par & change);
		if (take) {
			wib->nr_pending_stale -= nr;
			atomic_sub(nr, &wib->nr_stale);
		} else {
			wib->nr_pending_stale += nr;
			atomic_add(nr, &wib->nr_stale);
		}
	}
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * Stop answering from @pending: the recovery has taken the record over into
 * the live table, stripe by stripe, deciding for each what it still means --
 * or, with raid56_wf_recover_drops_refusals=1, is about to.  Idempotent.
 */
static void wib_stop_consulting_pending(struct btrfs_wib *wib)
{
	unsigned long flags;

	spin_lock_irqsave(&wib->lock, flags);
	if (wib->consult_pending) {
		wib->consult_pending = false;
		atomic_sub(wib->nr_pending_stale, &wib->nr_stale);
		wib->nr_pending_stale = 0;
	}
	WRITE_ONCE(wib->pending_unrecovered, false);
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * Is the full stripe at @full_stripe_start one the log read at mount recorded,
 * and that no recovery has taken over since?
 *
 * Only a read-only mount answers yes for more than the moment between reading
 * the log and recovering it, since it never recovers it.  And what it holds
 * then is exactly what the recovery exists for: a write listed in flight may
 * have landed on some devices and not others, so the parity may describe a
 * vector that never existed.  Nothing has checked that parity against the
 * data -- on a degraded array nothing can, scrub_raid56_recover_absent() would
 * have classified the stripe -- and a rebuild that has to spend every parity
 * on its missing members returns whatever that parity makes of them.  For data
 * without a checksum the reader takes that for the file's content.  The read
 * path asks this before it hands such a rebuild back (recover_rbio()), and
 * refuses it when the answer is yes.  A read-write mount's recovery takes the
 * record over but need not decide every stripe of it: one it keeps undecided
 * it keeps marked possibly torn, and the read path asks btrfs_wib_stripe_torn()
 * about that the same way.
 *
 * Only a record that says a write into the stripe may have been torn counts:
 * one in flight at the crash, or one marked possibly torn before it, which the
 * log writes as in flight (@torn in struct btrfs_wib_entry), or any error
 * record of a block whose writer did not mark, which cannot say which of them
 * is one (btrfs_wib_load_block()).  A record of a plain failed write names
 * what the failure left stale, the read has already treated that as missing,
 * and the parity it rebuilds from describes the acknowledged data: refusing
 * that rebuild costs a read that was right.  With
 * raid56_wf_all_records_torn=1 every record counts, as before the mark told
 * the two apart.  The records are only looked up for a rebuild that is about
 * to be returned unchecked, so this costs nothing on any other read.
 */
bool btrfs_wib_unrecovered(struct btrfs_fs_info *fs_info, u64 full_stripe_start,
			   int nr_data)
{
	const bool all = READ_ONCE(all_records_torn);
	struct btrfs_wib *wib = fs_info->wib;
	unsigned long flags;
	bool ret = false;

	if (!wib || !READ_ONCE(wib->pending_unrecovered))
		return false;

	spin_lock_irqsave(&wib->lock, flags);
	for (int i = 0; wib->pending_unrecovered && i < nr_data; i++) {
		const u64 logical = full_stripe_start +
				    ((u64)i << BTRFS_WIB_BLOCK_SHIFT);
		const u64 cur = wib_entry_bytenr(logical);
		const u64 mask = btrfs_wib_range_mask(cur, logical, BTRFS_WIB_BLOCK_SIZE);
		const struct btrfs_wib_entry *pe = wib_pending_block(wib, cur, mask);

		if (pe && ((pe->bitmap | pe->torn | (all ? pe->sticky : 0)) & mask)) {
			ret = true;
			break;
		}
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/*
 * Does the live table mark a block of [@start, @start + @len) possibly torn
 * (@torn in struct btrfs_wib_entry)?  Such a record is kept until the stripe
 * is rewritten or found consistent, which a repair that wrote no parity has
 * not done, and until then its parity is as unchecked as that of a stripe no
 * recovery has taken over: the read path refuses a rebuild with none left over
 * to check out of it (recover_rbio()), and scrub does not write one
 * (scrub_raid56_plan_wib()).  With raid56_wf_all_records_torn=1 nothing is
 * marked.
 */
bool btrfs_wib_stripe_torn(struct btrfs_fs_info *fs_info, u64 start, u64 len)
{
	struct btrfs_wib *wib = fs_info->wib;
	unsigned long flags;
	bool ret = false;

	if (!wib)
		return false;
	spin_lock_irqsave(&wib->lock, flags);
	for (u64 cur = wib_entry_bytenr(start); !ret && cur < start + len;
	     cur += BTRFS_WIB_ENTRY_SIZE) {
		const struct btrfs_wib_entry *e = wib_find_entry(wib, cur);

		ret = e && (e->torn & btrfs_wib_range_mask(cur, start, len));
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/*
 * Is the full stripe at @full_stripe_start the one the recovery is deciding
 * right now, a write into which may have been torn?  It took the stripe over
 * from @pending just before (btrfs_wib_take_pending()), so
 * btrfs_wib_unrecovered() no longer answers for it, and until its verdict is
 * in the live table does not mark it possibly torn either: what a read that
 * has to rebuild data without a checksum there, with no parity left over,
 * gets back is as unchecked as on a mount that never recovered it.  The read
 * path refuses it as such (recover_rbio()), unless it is the recovery's own.
 */
bool btrfs_wib_recovering(struct btrfs_fs_info *fs_info, u64 full_stripe_start)
{
	struct btrfs_wib *wib = fs_info->wib;
	unsigned long flags;
	bool ret;

	if (!wib || !READ_ONCE(wib->pending_unrecovered))
		return false;
	spin_lock_irqsave(&wib->lock, flags);
	ret = wib->recovering_len && wib->recovering == full_stripe_start;
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/* See @recovering in struct btrfs_wib; @len 0 when the verdict is in. */
static void wib_set_recovering(struct btrfs_wib *wib, u64 start, u64 len)
{
	unsigned long flags;

	spin_lock_irqsave(&wib->lock, flags);
	wib->recovering = start;
	wib->recovering_len = len;
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * The mount's recovery keeps the record of [@start, @start + @len), which it
 * loaded as possibly torn, because it could not resolve it: keep it marked, or
 * the next mount reads it as a plain failed write (see @torn in struct
 * btrfs_wib_entry).  After btrfs_wib_add_sticky() for the same range.
 */
static void wib_keep_torn(struct btrfs_wib *wib, u64 start, u64 len)
{
	unsigned long flags;

	if (READ_ONCE(all_records_torn))
		return;
	spin_lock_irqsave(&wib->lock, flags);
	for (u64 cur = wib_entry_bytenr(start); cur < start + len; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_entry *e = wib_find_entry(wib, cur);

		if (e) {
			e->torn |= btrfs_wib_range_mask(cur, start, len) & e->sticky;
			e->kept_torn |= btrfs_wib_range_mask(cur, start, len) & e->sticky;
		}
	}
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * The recovery found the full stripe [@start, @start + @len), which a write
 * may have torn, consistent but for @parities, on a missing device
 * (btrfs_scrub_raid56_full_stripe()): every data column read and verified,
 * every parity that is there regenerated from them.  A torn write is then
 * only in those parities, which nothing rewrote: record them stale, and the
 * stripe not possibly torn.  Marked so, a RAID6 read that has to rebuild a
 * sector from the parity just regenerated was refused (recover_rbio()) -- a
 * double fault RAID6 survives, turned into EIO for as long as the device
 * stayed out.  A mount with the device back regenerates the stale parity and
 * retires the record; one without a data device instead rebuilds from the
 * other parity, never from the stale one.  After btrfs_wib_add_sticky() for
 * the same range.
 *
 * Kept possibly torn (wib_keep_torn()) when there may be no room for the
 * mark: a stale parity makes a narrow log block wide, which holds half as
 * many regions, and the recovery still has the rest of @pending to put in the
 * live table -- spending records for the mark would be a worse trade.  Or with
 * raid56_wf_missing_parity_keeps_torn=1, as before.
 *
 * BTRFS_WIB_STRIPE_DECIDED instead of a parity: every parity is there, and
 * the missing device holds a data column the recovery decided -- both parities
 * agree about it, or it holds nothing there -- before it regenerated both
 * from the data (scrub_raid56_absent_pq()).  Nothing is left that a torn write
 * could be in, and nothing to record but the stripe, not possibly torn: a read
 * that loses a second column there rebuilds both from parities that describe
 * them.  With raid56_wf_absent_decided_keeps_torn=1 it stays possibly torn,
 * and that read fails, as before.
 */
EXPORT_FOR_TESTS
void btrfs_wib_parity_unwritten(struct btrfs_fs_info *fs_info, u64 start, u64 len,
				unsigned int parities)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool marks = parities & GENMASK(1, 0);
	unsigned long flags;
	bool room = true;

	if (marks) {
		spin_lock_irqsave(&wib->lock, flags);
		room = wib_live_max(wib) == BTRFS_WIB_MAX_ENTRIES ||
		       wib_live_count(wib) + wib->nr_pending <= BTRFS_WIB_MAX_ENTRIES;
		spin_unlock_irqrestore(&wib->lock, flags);
	}
	if (!room || (marks ? READ_ONCE(missing_parity_keeps_torn) :
			      READ_ONCE(absent_decided_keeps_torn))) {
		wib_keep_torn(wib, start, len);
		return;
	}
	for (int p = 0; p < 2; p++)
		if (parities & BIT(p))
			btrfs_wib_update_stale_parity(fs_info, start, p, true);
	spin_lock_irqsave(&wib->lock, flags);
	for (u64 cur = wib_entry_bytenr(start); cur < start + len; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_entry *e = wib_find_entry(wib, cur);

		if (e) {
			e->torn &= ~btrfs_wib_range_mask(cur, start, len);
			e->kept_torn &= ~btrfs_wib_range_mask(cur, start, len);
		}
	}
	spin_unlock_irqrestore(&wib->lock, flags);
}

/*
 * Gather what the log knows about one full stripe.
 *
 * Returns false when there is nothing recorded for it, which is the answer in
 * all but a vanishing fraction of calls and is reached without the lock.
 *
 * The caller must not act on @st without checking that the columns it cannot
 * believe fit within the parities it can still use.  Reconstructing more
 * columns than there are equations does not fail loudly for data without a
 * checksum: it returns a value nothing ever committed, which is worse than
 * the stale sector it was trying to avoid.
 */
bool btrfs_wib_stripe_state(struct btrfs_fs_info *fs_info, u64 full_stripe_start,
			    int nr_data, int nr_parity,
			    struct btrfs_wib_stripe_state *st)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;

	st->stale_cols = 0;
	st->bad_parity = 0;
	st->gen = 0;

	if (!wib)
		return false;
	/*
	 * nr_data above 64 cannot be expressed in one entry's bitmap.  No
	 * such chunk exists today; if one ever does, saying "nothing is
	 * recorded" is the behaviour without this record at all.
	 */
	if (nr_data > 64 || nr_parity > 2)
		return false;
	if (likely(!atomic_read(&wib->nr_stale))) {
		atomic64_inc(&wib->stat_stale_fast);
		return false;
	}
	atomic64_inc(&wib->stat_stale_slow);

	spin_lock_irqsave(&wib->lock, flags);
	for (int i = 0; i < nr_data; i++) {
		const u64 logical = full_stripe_start +
				    ((u64)i << BTRFS_WIB_BLOCK_SHIFT);
		const u64 cur = wib_entry_bytenr(logical);
		const u64 mask = btrfs_wib_range_mask(cur, logical, BTRFS_WIB_BLOCK_SIZE);
		const struct btrfs_wib_entry *e = wib_find_entry(wib, cur);
		const struct btrfs_wib_entry *pe = wib_find_pending(wib, cur, mask);
		u64 stale = 0, stale_par = 0, any = 0, gen = 0;

		if (e) {
			/*
			 * Not what a running replace marked for its target
			 * alone: the source still serves the stripe (see
			 * btrfs_wib_replace_mark_stale()).
			 */
			stale |= e->stale & ~e->replace_stale;
			stale_par |= e->stale_par & ~e->replace_stale_par;
			any |= e->stale | e->stale_par | e->sticky;
			gen = e->gen;
		}
		if (pe) {
			stale |= pe->stale;
			stale_par |= pe->stale_par;
			any |= pe->stale | pe->stale_par | pe->sticky;
			gen = max(gen, pe->gen);
		}
		if (stale & mask)
			st->stale_cols |= BIT_ULL(i);
		if (i < nr_parity && (stale_par & mask))
			st->bad_parity |= BIT(i);
		if (any & mask)
			st->gen = max(st->gen, gen);
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	return st->stale_cols || st->bad_parity;
}

/*
 * Does the log hold an error record for the full stripe at @full_stripe_start,
 * over all of its @nr_data blocks or only some?
 *
 * The record is set over whole full stripes, so a partial answer means part
 * of it was lost -- a stripe straddling two regions, one of which the log
 * dropped when it filled up.  Whatever the lost part knew about which side is
 * wrong is gone with it.
 */
enum btrfs_wib_stripe_error btrfs_wib_stripe_error(struct btrfs_fs_info *fs_info,
						   u64 full_stripe_start, int nr_data)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	int nr = 0;

	if (!wib)
		return BTRFS_WIB_STRIPE_NO_ERROR;

	spin_lock_irqsave(&wib->lock, flags);
	for (int i = 0; i < nr_data; i++) {
		const u64 logical = full_stripe_start +
				    ((u64)i << BTRFS_WIB_BLOCK_SHIFT);
		const u64 cur = wib_entry_bytenr(logical);
		const u64 mask = btrfs_wib_range_mask(cur, logical, BTRFS_WIB_BLOCK_SIZE);
		const struct btrfs_wib_entry *e = wib_find_entry(wib, cur);
		const struct btrfs_wib_entry *pe = wib_find_pending(wib, cur, mask);
		const u64 sticky = (e ? e->sticky : 0) | (pe ? pe->sticky : 0);

		if (sticky & mask)
			nr++;
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	if (nr == 0)
		return BTRFS_WIB_STRIPE_NO_ERROR;
	return nr == nr_data ? BTRFS_WIB_STRIPE_ERROR : BTRFS_WIB_STRIPE_PARTIAL_ERROR;
}

bool btrfs_wib_stale(struct btrfs_fs_info *fs_info, u64 logical)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 cur = wib_entry_bytenr(logical);
	struct btrfs_wib_entry *e;
	const struct btrfs_wib_entry *pe;
	bool stale = false;
	u64 mask;

	if (!wib)
		return false;
	/*
	 * The overwhelmingly common case: nothing anywhere is recorded stale,
	 * so answer without the lock or the table walk.  Racing with a record
	 * being added can only return the answer this function gave before
	 * the record existed at all, which is the behaviour without it.
	 */
	if (likely(!atomic_read(&wib->nr_stale))) {
		atomic64_inc(&wib->stat_stale_fast);
		return false;
	}
	atomic64_inc(&wib->stat_stale_slow);

	/*
	 * The block containing @logical and no other: @logical need not be
	 * block aligned (a read asks about each 4KiB sector), and a 64KiB
	 * range starting mid-block would also take in the NEXT block's bit.
	 */
	mask = btrfs_wib_range_mask(cur, round_down(logical, BTRFS_WIB_BLOCK_SIZE),
				    BTRFS_WIB_BLOCK_SIZE);
	spin_lock_irqsave(&wib->lock, flags);
	e = wib_find_entry(wib, cur);
	/* Leaving out a running replace's own marks, as btrfs_wib_stripe_state(). */
	if (e)
		stale = e->stale & ~e->replace_stale & mask;
	pe = wib_find_pending(wib, cur, mask);
	if (pe)
		stale |= pe->stale & mask;
	spin_unlock_irqrestore(&wib->lock, flags);
	return stale;
}

/*
 * Copy the regions the log holds a fault record for into @out, lowest address
 * first, skipping anything below @from.  @out must have room for
 * BTRFS_WIB_NR_ENTRIES.  Returns how many were written.
 *
 * Only regions with a fault record are reported.  An entry that merely has
 * writes in flight is an ordinary write happening right now, and on a healthy
 * array that is nearly the whole table; a tool looking for damage would have
 * to filter it out again, and would have to do so on a snapshot that is
 * already stale.  What crashed mid-write is not lost by this: the next mount
 * recovers those stripes and keeps a fault record for any it could not finish.
 */
int btrfs_wib_snapshot(struct btrfs_fs_info *fs_info, u64 from,
		       struct btrfs_wib_entry *out)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	int nr = 0;

	if (!wib)
		return 0;

	spin_lock_irqsave(&wib->lock, flags);
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		const struct btrfs_wib_entry *e = &wib->entries[i];

		if (!wib_entry_used(e) || !e->sticky)
			continue;
		if (e->bytenr < from)
			continue;
		out[nr++] = *e;
	}
	spin_unlock_irqrestore(&wib->lock, flags);

	sort(out, nr, sizeof(*out), wib_pending_cmp, NULL);
	return nr;
}

void btrfs_wib_clear_sticky(struct btrfs_fs_info *fs_info, u64 logical, u64 len)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 end = logical + len;
	bool freed = false;

	if (!wib)
		return;

	spin_lock_irqsave(&wib->lock, flags);
	/*
	 * Not while a running device replace has marks of its own here (see
	 * @replace_stale, @replace_keep in struct btrfs_wib_entry).  They describe the zeros
	 * on its target, which the caller has neither looked at nor rewritten
	 * -- the queries it decided by leave them out -- and retiring the
	 * record would leave those zeros to be read as data once the target
	 * takes over.  The record stays for btrfs_wib_replace_end() to hand
	 * over or drop, and for the next scrub after that.
	 */
	for (u64 cur = wib_entry_bytenr(logical); cur < end; cur += BTRFS_WIB_ENTRY_SIZE) {
		const struct btrfs_wib_entry *e = wib_find_entry(wib, cur);
		const u64 mask = btrfs_wib_range_mask(cur, logical, len);

		if (e && wib_entry_replace_kept(e, mask)) {
			spin_unlock_irqrestore(&wib->lock, flags);
			return;
		}
	}
	for (u64 cur = wib_entry_bytenr(logical); cur < end; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_entry *e = wib_find_entry(wib, cur);

		if (!e)
			continue;
		e->sticky &= ~btrfs_wib_range_mask(cur, logical, len);
		if (e->stale & btrfs_wib_range_mask(cur, logical, len)) {
			atomic_sub(hweight64(e->stale &
					     btrfs_wib_range_mask(cur, logical, len)),
				   &wib->nr_stale);
			e->stale &= ~btrfs_wib_range_mask(cur, logical, len);
		}
		/*
		 * The stripe is consistent again, so its parity describes the
		 * data.  Left set, this bit would be persisted and would then
		 * make a later rebuild refuse a parity that is in fact fine --
		 * conservative rather than dangerous, but wrong, and it does
		 * not clear itself.
		 */
		wib_set_stale_par(wib, e, e->stale_par &
				  ~btrfs_wib_range_mask(cur, logical, len));
		e->hold &= ~btrfs_wib_range_mask(cur, logical, len);
		e->hold_par &= ~btrfs_wib_range_mask(cur, logical, len);
		/* Rewritten or found consistent: nothing in it is torn. */
		e->torn &= ~btrfs_wib_range_mask(cur, logical, len);
		e->kept_torn &= ~btrfs_wib_range_mask(cur, logical, len);
		wib_replace_disown(e, btrfs_wib_range_mask(cur, logical, len),
				   btrfs_wib_range_mask(cur, logical, len));
		e->replace_keep &= ~btrfs_wib_range_mask(cur, logical, len);
		if (!wib_entry_used(e))
			freed = true;
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	if (freed)
		wake_up_all(&wib->wait);
	/* That may have been the last one: say so now, not in a minute. */
	if (READ_ONCE(wib->health) != BTRFS_RAID56_HEALTH_OK)
		raid56_alert_kick(wib, 0);
}

/*
 * Forget every record for [@logical, @logical + @len): the address has stopped
 * meaning anything, because the chunk that gave it a geometry is gone.
 *
 * Not the same thing as btrfs_wib_clear_sticky(), even though it clears the
 * same fields.  That one says "this stripe is consistent again", which is a
 * statement about data.  This one says "there is no stripe here any more",
 * which is a statement about the address -- and leaving the record behind is
 * not the conservative choice it looks like.  A record is interpreted against
 * whatever chunk covers its address at the time it is read, so once the
 * address is reallocated the surviving bits are read with a different
 * @nr_data, a different column-to-device mapping, or no chunk at all.  A
 * scrub then believes a column of the NEW chunk is stale and rebuilds it from
 * a parity that was in fact describing it correctly: the record meant to stop
 * a misrepair causes one.
 *
 * An in-flight bit here would mean a write was still outstanding against a
 * chunk being removed, which is a bug somewhere else; say so rather than
 * quietly clearing it, and clear it anyway, because keeping it would pin an
 * entry that nothing will ever complete.
 */
/*
 * The evidence channel.
 *
 * A scrub that declines to repair an ambiguous full stripe is holding, at that
 * instant, the only mutually coherent copy of its data columns that anything
 * will ever have: freshly read, read-only for the whole chunk scrub so nothing
 * can be writing, and about to be dropped.  Copy them for a helper that is
 * listening, and only then -- an unwatched filesystem must pay nothing.
 *
 * The parity is deliberately NOT copied.  Every column's devid and physical
 * offset is handed over instead, and the block group stays read-only until the
 * chunk's scrub finishes, so a helper reads the parity off the device without
 * racing anything.  The alternative is issuing more reads to an array that is
 * failing by construction, on a path with no REQ_FAILFAST and no timeout.
 */
/*
 * A hint, deliberately taken without the lock: it exists so that a filesystem
 * nobody is watching pays one load per declined stripe rather than a mutex.
 * It can go stale the instant it is read, so it is only ever used to skip work
 * -- btrfs_raid56_evidence_claim() decides for real, under the lock.
 */
bool btrfs_raid56_evidence_armed(const struct btrfs_fs_info *fs_info)
{
	return READ_ONCE(fs_info->raid56_evidence) != NULL;
}

#ifdef CONFIG_BTRFS_DEBUG
/*
 * Lower the column count at which the channel refuses to copy a stripe.
 *
 * A stripe is refused when it has more columns than the header can name
 * (BTRFS_RAID56_EVIDENCE_MAX_COLS, 34) or more data than a slot can hold
 * (BTRFS_RAID56_EVIDENCE_MAX_BYTES, sixteen columns' worth).  The second is
 * the one a real array hits first: a RAID5 exceeds it at eighteen devices.
 * UML tops out at sixteen block devices, so neither refusal, nor the
 * dropped_wide counter that reports it, could be reached by any test on this
 * rig.  With this set, an ordinary four-device array reaches the first.
 */
static unsigned int evidence_max_cols;
module_param_named(raid56_evidence_max_cols, evidence_max_cols, uint, 0644);
MODULE_PARM_DESC(raid56_evidence_max_cols,
		 "Refuse to copy a stripe wider than this many columns; 0 for the real limit (testing only)");
#endif

/*
 * How long a capture will wait for the helper to free a slot before giving up
 * on the stripe.
 *
 * Measured, before this existed: a scrub declining nine stripes with a helper
 * draining once a second kept four and dropped five.  The stripes arrive in
 * bursts, faster than any poll interval, so "drain more often" cannot fix it
 * -- the capture has to be willing to wait.  Scrub is background work and this
 * is the one moment those bytes exist together, so a few seconds of scrub is a
 * good trade for a full stripe of evidence.
 *
 * The total is bounded, and a capture that waits it out in vain sets ->stalled
 * so the rest of the scrub does not pay it again: a helper that died costs one
 * grace period, not one per declined stripe.
 */
#ifdef CONFIG_BTRFS_DEBUG
/*
 * Hold the window between claiming a slot and publishing it open for this
 * long.
 *
 * That window is the one the locking rework is about: a disarm landing inside
 * it used to clear the channel pointer, block on the channel's own lock, and
 * never be woken, because the capture then saw a NULL pointer and returned
 * without releasing that lock.  It is a memcpy wide, so a test can only hope
 * to hit it; with this set it hits it every time.
 */
static unsigned int evidence_capture_delay_ms;
module_param_named(raid56_evidence_capture_delay_ms, evidence_capture_delay_ms, uint, 0644);
MODULE_PARM_DESC(raid56_evidence_capture_delay_ms,
		 "Hold a claimed evidence slot this long before publishing it (testing only)");
#endif

#define EVIDENCE_WAIT_STEP_MS	50
static unsigned int evidence_wait_ms = 5000;
module_param_named(raid56_evidence_wait_ms, evidence_wait_ms, uint, 0644);
MODULE_PARM_DESC(raid56_evidence_wait_ms,
		 "How long a declined stripe waits for a free evidence slot, 0 to drop immediately");

static u32 evidence_col_limit(void)
{
#ifdef CONFIG_BTRFS_DEBUG
	unsigned int v = READ_ONCE(evidence_max_cols);

	if (v && v < BTRFS_RAID56_EVIDENCE_MAX_COLS)
		return v;
#endif
	return BTRFS_RAID56_EVIDENCE_MAX_COLS;
}

static void evidence_free(struct btrfs_raid56_evidence *ev)
{
	if (!ev)
		return;
	for (int i = 0; i < BTRFS_RAID56_EVIDENCE_SLOTS; i++)
		kvfree(ev->slots[i].data);
	kfree(ev);
}

/*
 * Decide an ARM against a channel that is already armed.  Called with the lock
 * held.  @owner is the file asking to bind, or NULL for an unbound ARM.
 */
static int evidence_rearm_locked(struct btrfs_raid56_evidence *ev,
				 const struct file *owner)
{
	/* Bound to someone else: that helper owns the channel's lifetime. */
	if (ev->owner && ev->owner != owner)
		return -EBUSY;
	/* Unbound and this ARM binds: claim it. */
	if (!ev->owner && owner)
		ev->owner = owner;
	return 0;
}

int btrfs_raid56_evidence_arm(struct btrfs_fs_info *fs_info, const struct file *owner)
{
	struct btrfs_raid56_evidence *ev;
	int ret;

	/*
	 * Already armed is the common repeat case; answer it without first
	 * allocating four megabytes to throw away.
	 */
	mutex_lock(&fs_info->raid56_evidence_lock);
	if (fs_info->raid56_evidence) {
		ret = evidence_rearm_locked(fs_info->raid56_evidence, owner);
		mutex_unlock(&fs_info->raid56_evidence_lock);
		return ret;
	}
	mutex_unlock(&fs_info->raid56_evidence_lock);

	/*
	 * Allocate outside the lock, and drop it again if someone else armed
	 * in the meantime.  Testing the pointer and allocating after, with
	 * nothing in between, left two concurrent arms both allocating and the
	 * loser's slots unreachable for the life of the mount.
	 */
	ev = kzalloc(sizeof(*ev), GFP_KERNEL);
	if (!ev)
		return -ENOMEM;
	for (int i = 0; i < BTRFS_RAID56_EVIDENCE_SLOTS; i++) {
		ev->slots[i].data = kvmalloc(BTRFS_RAID56_EVIDENCE_MAX_BYTES,
					     GFP_KERNEL);
		if (!ev->slots[i].data) {
			evidence_free(ev);
			return -ENOMEM;
		}
	}
	ev->owner = owner;

	mutex_lock(&fs_info->raid56_evidence_lock);
	if (fs_info->raid56_evidence) {
		ret = evidence_rearm_locked(fs_info->raid56_evidence, owner);
		mutex_unlock(&fs_info->raid56_evidence_lock);
		evidence_free(ev);
		return ret;
	}
	fs_info->raid56_evidence = ev;
	mutex_unlock(&fs_info->raid56_evidence_lock);
	btrfs_info(fs_info,
		   "raid56: evidence channel armed%s, %u slots of %u bytes",
		   owner ? " and bound to its reader" : "",
		   BTRFS_RAID56_EVIDENCE_SLOTS,
		   (unsigned int)BTRFS_RAID56_EVIDENCE_MAX_BYTES);
	return 0;
}

/*
 * Free a channel that has already been unpublished, saying what it cost.
 *
 * Throwing away queued entries is the one loss here nothing can undo
 * afterwards: the stripe is not read again, and the scrub that held its
 * columns together has finished.  Refusing to disarm is not an option -- a
 * helper that died has to be able to have its memory freed, and this is also
 * the unmount path -- so say it instead of losing them quietly.
 */
static void evidence_retire(struct btrfs_fs_info *fs_info,
			    struct btrfs_raid56_evidence *ev, const char *why)
{
	if (!ev)
		return;
	if (unlikely(ev->nr))
		/*
		 * Rate limited, not because it is unimportant but because
		 * ARM/DISARM is a userspace loop, and an unratelimited warning
		 * on it is a way to fill the kernel log from userspace.  The
		 * first of a burst is what an operator needs.
		 */
		btrfs_warn_rl(fs_info,
"raid56: evidence channel disarmed with %u captured stripe(s) still unread (%s); they are discarded",
			      ev->nr, why);
	evidence_free(ev);
}

/*
 * Unpublish under the lock, free outside it.  Every capture and every read
 * runs wholly under the same lock, so once it is released here none of them
 * can be holding a reference: they either took the lock before this and
 * finished, or take it after and find NULL.
 *
 * The lock being in fs_info rather than in the channel is what makes that
 * true.  With the lock inside the object, a capture that had read the pointer
 * but not yet taken the lock was invisible to any handshake done here, and
 * locked freed memory a moment later.
 */
int btrfs_raid56_evidence_disarm_request(struct btrfs_fs_info *fs_info, bool if_empty)
{
	struct btrfs_raid56_evidence *ev;

	mutex_lock(&fs_info->raid56_evidence_lock);
	ev = fs_info->raid56_evidence;
	if (ev && if_empty && ev->nr) {
		mutex_unlock(&fs_info->raid56_evidence_lock);
		return -EBUSY;
	}
	fs_info->raid56_evidence = NULL;
	mutex_unlock(&fs_info->raid56_evidence_lock);
	evidence_retire(fs_info, ev, "by DISARM");
	return 0;
}

/* Teardown: unconditional, from btrfs_wib_free() at unmount. */
void btrfs_raid56_evidence_disarm(struct btrfs_fs_info *fs_info)
{
	struct btrfs_raid56_evidence *ev;

	mutex_lock(&fs_info->raid56_evidence_lock);
	ev = fs_info->raid56_evidence;
	fs_info->raid56_evidence = NULL;
	mutex_unlock(&fs_info->raid56_evidence_lock);
	evidence_retire(fs_info, ev, "at unmount");
}

/*
 * A btrfs file is being released for the last time.  If it is the one an
 * ARM_BIND tied the channel to, its reader is gone -- closed it, exited, or
 * was killed -- and nobody is left to drain.  Disarm now, and say how much
 * that discarded, rather than leave the ring to fill and the captures after
 * it to drop into a counter only the dead reader would have read.
 *
 * Called on every btrfs file release, so the unarmed case must cost nothing:
 * the lockless hint first, the lock only when something is armed.  No ioctl
 * can be in flight on @file while it is released -- the ioctl path holds a
 * reference -- so an ARM_BIND on this same file cannot race this.
 */
void btrfs_raid56_evidence_file_released(struct btrfs_fs_info *fs_info,
					 const struct file *file)
{
	struct btrfs_raid56_evidence *ev;

	if (likely(!btrfs_raid56_evidence_armed(fs_info)))
		return;

	mutex_lock(&fs_info->raid56_evidence_lock);
	ev = fs_info->raid56_evidence;
	if (!ev || ev->owner != file) {
		mutex_unlock(&fs_info->raid56_evidence_lock);
		return;
	}
	fs_info->raid56_evidence = NULL;
	mutex_unlock(&fs_info->raid56_evidence_lock);
	btrfs_info(fs_info, "raid56: evidence channel's reader closed it; disarming");
	evidence_retire(fs_info, ev, "its reader went away");
}

/*
 * Reserve the slot the next capture will fill, or NULL if there is nothing to
 * fill it into.  The caller copies into slot->data and then calls
 * btrfs_raid56_evidence_commit() to publish it.
 *
 * The ring never overwrites a queued entry.  Dropping the OLDEST evidence to
 * make room for the newest would be the wrong way round: the helper is
 * draining in order, and the entry it has not read yet is the one it is about
 * to.  A full ring means the helper is not keeping up, which is a fact worth
 * reporting rather than papering over.
 */
struct btrfs_raid56_evidence_slot *
btrfs_raid56_evidence_claim(struct btrfs_fs_info *fs_info, u32 nr_data, u32 nr_parity)
{
	struct btrfs_raid56_evidence *ev;
	struct btrfs_raid56_evidence_slot *slot;
	unsigned int waited_ms = 0;
	bool waited = false;

	for (;;) {
		mutex_lock(&fs_info->raid56_evidence_lock);
		ev = fs_info->raid56_evidence;
		if (!ev) {
			mutex_unlock(&fs_info->raid56_evidence_lock);
			return NULL;
		}
		if (nr_data + nr_parity > evidence_col_limit() ||
		    (u64)nr_data * BTRFS_STRIPE_LEN > BTRFS_RAID56_EVIDENCE_MAX_BYTES) {
			ev->dropped_wide++;
			mutex_unlock(&fs_info->raid56_evidence_lock);
			return NULL;
		}
		if (ev->nr < BTRFS_RAID56_EVIDENCE_SLOTS)
			break;
		/*
		 * Full.  The oldest entry is the one the helper is about to
		 * read, so it is never the one to throw away -- wait for the
		 * helper instead, and only drop this stripe once waiting has
		 * stopped being worth it.
		 */
		if (ev->stalled || waited_ms >= READ_ONCE(evidence_wait_ms)) {
			/*
			 * Counted here as well as on the success path: ->waited
			 * is "captures that had to sleep", so that a helper
			 * which died shows up as ONE of them for the whole
			 * scrub.  Counting only the ones that went on to get a
			 * slot would make the ->stalled bound -- the thing that
			 * keeps a dead helper from costing a grace period per
			 * declined stripe -- unobservable from outside.
			 */
			if (waited)
				ev->waited++;
			ev->stalled = true;
			ev->dropped_full++;
			mutex_unlock(&fs_info->raid56_evidence_lock);
			return NULL;
		}
		mutex_unlock(&fs_info->raid56_evidence_lock);
		msleep(EVIDENCE_WAIT_STEP_MS);
		waited_ms += EVIDENCE_WAIT_STEP_MS;
		waited = true;
	}
	if (waited)
		ev->waited++;
	slot = &ev->slots[(ev->head + ev->nr) % BTRFS_RAID56_EVIDENCE_SLOTS];
	memset(slot, 0, offsetof(struct btrfs_raid56_evidence_slot, data));
	slot->nr_data = nr_data;
	slot->nr_parity = nr_parity;
#ifdef CONFIG_BTRFS_DEBUG
	if (unlikely(READ_ONCE(evidence_capture_delay_ms)))
		msleep(READ_ONCE(evidence_capture_delay_ms));
#endif
	/* Held across the copy; commit() releases it. */
	return slot;
}

/*
 * Publish the slot claimed above and release the lock.  Every path out of
 * btrfs_raid56_evidence_claim() that returned a slot MUST reach here.
 *
 * This used to re-read fs_info->raid56_evidence and return early when it was
 * NULL, which is the one case where returning early is fatal: a disarm that
 * had just cleared the pointer was by then blocked on the very lock this call
 * was supposed to release, and never woke up.
 */
void btrfs_raid56_evidence_commit(struct btrfs_fs_info *fs_info)
{
	struct btrfs_raid56_evidence *ev = fs_info->raid56_evidence;

	lockdep_assert_held(&fs_info->raid56_evidence_lock);
	/* Cannot have been disarmed: disarm needs the lock we are holding. */
	ASSERT(ev);
	ev->nr++;
	ev->captured++;
	mutex_unlock(&fs_info->raid56_evidence_lock);
}

int btrfs_raid56_evidence_take(struct btrfs_fs_info *fs_info,
			       struct btrfs_ioctl_raid56_evidence_args *args,
			       void __user *ubuf)
{
	struct btrfs_raid56_evidence *ev;
	struct btrfs_raid56_evidence_slot *slot;
	void *copy;
	u32 nr_bytes;
	int ret = 0;

	/*
	 * Allocate the bounce buffer first: the pointer is only meaningful
	 * while the lock is held, so everything that can sleep for its own
	 * reasons happens either side of it.
	 */
	copy = kvmalloc(BTRFS_RAID56_EVIDENCE_MAX_BYTES, GFP_KERNEL);
	if (!copy)
		return -ENOMEM;

	mutex_lock(&fs_info->raid56_evidence_lock);
	ev = fs_info->raid56_evidence;
	if (!ev) {
		mutex_unlock(&fs_info->raid56_evidence_lock);
		kvfree(copy);
		return -ENODEV;
	}
	/*
	 * Somebody is reading, so waiting for a slot is worth doing again.
	 * Cleared on every read, including the ones that find nothing queued:
	 * that is exactly a helper polling an empty ring, which is the state
	 * this is meant to trust.
	 */
	ev->stalled = false;
	args->waited = ev->waited;
	args->dropped_full = ev->dropped_full;
	args->dropped_wide = ev->dropped_wide;
	args->captured = ev->captured;
	if (!ev->nr) {
		args->nr_queued = 0;
		args->buf_size = 0;
		args->nr_data = 0;
		mutex_unlock(&fs_info->raid56_evidence_lock);
		kvfree(copy);
		return -ENOENT;
	}
	slot = &ev->slots[ev->head];
	nr_bytes = slot->nr_bytes;
	if (nr_bytes > args->buf_size) {
		/* Say how much is needed and leave the entry queued. */
		args->buf_size = nr_bytes;
		args->nr_queued = ev->nr;
		mutex_unlock(&fs_info->raid56_evidence_lock);
		kvfree(copy);
		return -ERANGE;
	}
	args->full_stripe_start = slot->full_stripe_start;
	args->gen = slot->gen;
	args->stale_cols = slot->stale_cols;
	args->bad_parity = slot->bad_parity;
	args->record_flags = slot->record_flags;
	args->nr_data = slot->nr_data;
	args->nr_parity = slot->nr_parity;
	args->stripe_len = BTRFS_STRIPE_LEN;
	memcpy(args->devid, slot->devid, sizeof(args->devid));
	memcpy(args->physical, slot->physical, sizeof(args->physical));
	memcpy(copy, slot->data, nr_bytes);
	ev->head = (ev->head + 1) % BTRFS_RAID56_EVIDENCE_SLOTS;
	ev->nr--;
	args->nr_queued = ev->nr;
	args->buf_size = nr_bytes;
	mutex_unlock(&fs_info->raid56_evidence_lock);

	if (nr_bytes && copy_to_user(ubuf, copy, nr_bytes))
		ret = -EFAULT;
	kvfree(copy);
	return ret;
}

void btrfs_wib_forget_range(struct btrfs_fs_info *fs_info, u64 logical, u64 len)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 end = logical + len;
	unsigned int inflight = 0;
	bool freed = false;

	if (!wib)
		return;

	spin_lock_irqsave(&wib->lock, flags);
	for (u64 cur = wib_entry_bytenr(logical); cur < end; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_entry *e = wib_find_entry(wib, cur);
		u64 mask;

		if (!e)
			continue;
		mask = btrfs_wib_range_mask(cur, logical, len);
		inflight += hweight64(e->bitmap & mask);
		e->bitmap &= ~mask;
		e->sticky &= ~mask;
		if (e->stale & mask) {
			atomic_sub(hweight64(e->stale & mask), &wib->nr_stale);
			e->stale &= ~mask;
		}
		wib_set_stale_par(wib, e, e->stale_par & ~mask);
		e->hold &= ~mask;
		e->hold_par &= ~mask;
		e->torn &= ~mask;
		e->kept_torn &= ~mask;
		wib_replace_disown(e, mask, mask);
		e->replace_keep &= ~mask;
		if (!wib_entry_used(e)) {
			e->gen = 0;
			freed = true;
		}
	}
	for (u64 cur = wib_entry_bytenr(logical); cur < end; cur += BTRFS_WIB_ENTRY_SIZE) {
		struct btrfs_wib_written *w = wib_written_find(wib, cur);
		const u64 mask = btrfs_wib_range_mask(cur, logical, len);

		if (w) {
			w->cols &= ~mask;
			w->par &= ~mask;
		}
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	if (inflight)
		btrfs_warn(fs_info,
"raid56 write-intent log: %u block(s) of the chunk at %llu were still recorded in flight as it was removed",
			   inflight, logical);
	if (freed)
		wake_up_all(&wib->wait);
}

/*
 * Called at transaction commit (and log commit) time, before the device
 * barriers: snapshot the in-flight set, so that btrfs_wib_commit() only
 * drops the stripes that finished before the barriers were issued.
 */
void btrfs_wib_commit_prepare(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	int ret;

	if (!wib)
		return;
	mutex_lock(&wib->commit_mutex);
	ret = btrfs_wib_build_block(wib, wib->prepared, 0, NULL);
	ASSERT(ret == 0);
	wib->prepared_valid = true;
	mutex_unlock(&wib->commit_mutex);
}

/*
 * Make the in-memory record durable now, dropping what has finished: every
 * device is flushed first, then the current set is written.  For a
 * read-modify-write that has just put a stale column back (with FUA) and
 * cleared its mark, and must not change the parity until the log on disk has
 * stopped naming the column -- see rmw_repair_first() in raid56.c.
 *
 * -EIO if a device did not confirm the flush: nothing is written then, and the
 * block on disk, a superset of the truth, stays as it was.
 */
int btrfs_wib_persist_now(struct btrfs_fs_info *fs_info)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	bool enabled;
	u64 seq;
	int ret;

	if (!wib)
		return 0;
	spin_lock_irqsave(&wib->lock, flags);
	enabled = wib->enabled;
	spin_unlock_irqrestore(&wib->lock, flags);
	if (!enabled)
		return 0;

	mutex_lock(&wib->commit_mutex);
	spin_lock_irqsave(&wib->lock, flags);
	seq = ++wib->snap_seq;
	spin_unlock_irqrestore(&wib->lock, flags);
	/*
	 * An earlier failed flush left stripes the set has not taken back,
	 * and until it has nothing may be dropped -- so the log on disk cannot
	 * stop naming anything either.  The writes in flight that held the
	 * room may have finished since the last commit planned it; plan
	 * again rather than wait for the next one, which on an idle
	 * filesystem may be a long time coming.
	 */
	if (wib->readd_owed)
		wib_readd_dropped(wib, NULL, NULL);
	if (wib->readd_owed)
		ret = -EIO;
	else
		ret = btrfs_wib_build_block(wib, wib->flushsnap, seq, NULL);
	if (ret == 0) {
		const int flush = wib_flush_all_devices(wib, &wib->flush_failed);

		if (flush == 0) {
			ret = wib_drop_locked(wib, seq, wib->flushsnap, true, NULL, true,
					      true);
		} else {
			/*
			 * Nothing is written, but only this flush knows which
			 * device may have lost the writes the last block
			 * lists; the next commit's barrier may well succeed
			 * and drop them.  Take them back now, naming it.
			 */
			if (!READ_ONCE(readd_legacy))
				wib_readd_dropped(wib, &wib->flush_failed,
						  flush > 0 ? wib->flushsnap : NULL);
			ret = -EIO;
		}
	}
	mutex_unlock(&wib->commit_mutex);
	return ret;
}

/*
 * Overwrite @nr_slots of every device with the current in-memory set, each
 * write preceded by a flush.  Writing every slot leaves no older block behind
 * that a later mount could pick as the newest one; writing a single slot is
 * enough when only the newest block matters.  commit_mutex must be held.
 */
static int wib_persist_all_slots_locked(struct btrfs_wib *wib, int nr_slots)
{
	unsigned long flags;
	int ret = 0;

	lockdep_assert_held(&wib->commit_mutex);

	for (int i = 0; i < nr_slots; i++) {
		u64 seq;

		spin_lock_irqsave(&wib->lock, flags);
		seq = ++wib->snap_seq;
		spin_unlock_irqrestore(&wib->lock, flags);
		ret = wib_flush_and_drop_locked(wib, seq, true);
		if (ret < 0)
			break;
	}
	if (ret < 0)
		btrfs_err(wib->fs_info,
			  "raid56 write-intent log: failed to write the log: %d",
			  ret);
	return ret;
}

static int wib_persist_all_slots(struct btrfs_wib *wib, int nr_slots)
{
	int ret;

	mutex_lock(&wib->commit_mutex);
	ret = wib_persist_all_slots_locked(wib, nr_slots);
	mutex_unlock(&wib->commit_mutex);
	return ret;
}

/*
 * The filesystem is being unmounted, its last transaction committed and its
 * repairs stopped (close_ctree()): no write is in flight any more.  The log
 * may still list some in flight, though, and nothing drops them but a flush
 * every device confirms: a write recorded after the last transaction commit
 * -- a repair (btrfs_raid56_repair_work()) takes no transaction, and when none
 * is running the unmount commits none (btrfs_commit_current_transaction()) --
 * or one that finished before a commit whose barrier a device failed, which
 * a readd could not take back as an error record (wib_readd_base()).  Left
 * there, each reads to the next mount as a write a crash may have torn: its
 * stripe's parity goes unchecked, and where the record names a column only
 * that parity can rebuild, the recovery keeps the stripe undecided and its
 * reads fail (SCRUB_WIB_TORN), for a write that finished before the unmount
 * did.  So flush and write the set once more, as a transaction commit does:
 * what finished drops, the records stay, and so does a possibly torn mark,
 * which the set writes as in flight.  A device that fails the flush is named
 * as at any commit (wib_readd_dropped()).  Only when the last block lists in
 * flight a block the set does not: otherwise the flush would change nothing.
 */
void btrfs_wib_unmount(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const struct btrfs_wib_disk_header *hdr;
	unsigned long flags;
	bool stray = false;
	bool enabled;

	if (!wib || READ_ONCE(finished_stay_inflight))
		return;
	spin_lock_irqsave(&wib->lock, flags);
	enabled = wib->enabled;
	spin_unlock_irqrestore(&wib->lock, flags);
	if (!enabled)
		return;

	mutex_lock(&wib->commit_mutex);
	hdr = wib->last;
	if (le64_to_cpu(hdr->magic) == BTRFS_WIB_MAGIC) {
		const u32 nr = min(le32_to_cpu(hdr->nr_entries),
				   wib_block_max_entries(wib->last));

		spin_lock_irqsave(&wib->lock, flags);
		for (u32 i = 0; i < nr && !stray; i++) {
			const struct btrfs_wib_entry *live;
			struct btrfs_wib_entry e;

			btrfs_wib_read_entry(wib->last, i, &e);
			live = wib_find_entry(wib, e.bytenr);
			stray = e.bitmap & ~(live ? live->bitmap | live->torn : 0);
		}
		spin_unlock_irqrestore(&wib->lock, flags);
	}
	if (stray)
		wib_persist_all_slots_locked(wib, 1);
	mutex_unlock(&wib->commit_mutex);
}

/*
 * The filesystem is being remounted read-only, its last transaction committed
 * and its repairs stopped (btrfs_remount_ro()).  To the log this is an
 * unmount: nothing writes it again before a read-write mount, and an unmount
 * of the read-only filesystem writes nothing (close_ctree()), so what it lists
 * in flight now is what a crash, or the next mount, reads.
 */
void btrfs_wib_remount_ro(struct btrfs_fs_info *fs_info)
{
	if (READ_ONCE(remount_ro_keeps_inflight))
		return;
	btrfs_wib_unmount(fs_info);
}

/*
 * The log is disabled (wib->enabled is clear) at a transaction commit:
 * write its last block, to every slot.  @flushed: every device confirmed the
 * commit's barrier.  commit_mutex must be held.
 *
 * Nothing refreshes the log from here on, so whatever block the devices carry
 * is the one they keep -- and it lists the stripes that were in flight at some
 * earlier commit, which have long since completed.  The next mount reads it
 * (btrfs_wib_load() does not look at the feature flag) and scrubs every stripe
 * in it, so leaving a stale block turns a disable into a slow mount later on,
 * for stripes that are fine.  Write the current set once instead: the writes
 * still in flight, and the stripes recorded as damaged, which do want that
 * scrub.
 *
 * That is a drop like any other, and a device that did not confirm the flush
 * it relies on may have lost what it drops: the records a failed flush keeps
 * name it as they would with the log enabled (wib_readd_dropped()), which
 * takes the written records.  So they are forgotten only once the block is
 * written, and then only as far as no readd is still owed
 * (wib_written_reset_locked()).  Two flushes are in question:
 *
 *  - the commit's barrier.  The commit would have taken back the stripes it
 *    was about to drop, naming whoever failed it (wib_drop_locked()); the
 *    flushes below may well succeed and drop them unnamed.  Take them back
 *    first, from the written records as they stand: every write issued
 *    before the barrier noted itself, the log being enabled then;
 *  - the flushes below.  They also cover the writes issued since the log was
 *    disabled, and those noted nothing (btrfs_wib_note_written()): for them
 *    every member counts as written, which names more than it has to and
 *    never less.
 *
 * raid56_wf_disable_forgets_writes=1 forgets the written records first and
 * ignores the barrier, as the disable did before: a failed flush here then
 * names nothing, marks nothing possibly torn, and raises no alert.
 */
static int wib_write_final_locked(struct btrfs_wib *wib, bool flushed)
{
	unsigned long flags;
	int ret;

	lockdep_assert_held(&wib->commit_mutex);

	if (READ_ONCE(disable_forgets_writes)) {
		spin_lock_irqsave(&wib->lock, flags);
		wib_written_reset_locked(wib);
		spin_unlock_irqrestore(&wib->lock, flags);
	} else {
		if (!flushed) {
			wib_barrier_failed(wib->fs_info, &wib->flush_failed);
			wib_readd_dropped(wib, &wib->flush_failed,
					  wib->prepared_valid ? wib->prepared : NULL);
		}
		spin_lock_irqsave(&wib->lock, flags);
		wib->written_unknown = true;
		spin_unlock_irqrestore(&wib->lock, flags);
	}
	/* This commit drops nothing against its snapshot. */
	wib->prepared_valid = false;
	ret = wib_persist_all_slots_locked(wib, BTRFS_WIB_NR_SLOTS);
	spin_lock_irqsave(&wib->lock, flags);
	wib_written_reset_locked(wib);
	spin_unlock_irqrestore(&wib->lock, flags);
	return ret;
}

/*
 * Called at transaction commit (and log commit) time, after the device
 * barriers and before the superblocks are written.  Persists the current
 * in-flight set, dropping stripes that finished before the snapshot taken
 * by btrfs_wib_commit_prepare() (@flushed: every device confirmed the
 * barrier), handles an enable requested from a context that could not do
 * IO itself, and a pending disable.
 */
int btrfs_wib_commit(struct btrfs_fs_info *fs_info, bool flushed)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	bool enabled;
	bool enable;
	bool disable;
	bool timed;
	u64 seq;
	int ret;

	if (!wib)
		return 0;

	spin_lock_irqsave(&wib->lock, flags);
	enabled = wib->enabled;
	enable = wib->enable_requested;
	disable = wib->disable_requested;
	/*
	 * Tell a concurrent btrfs_wib_disable() that an enable it cannot see
	 * in wib->enabled is under way, so that it is not lost between here
	 * and the re-check below.
	 */
	if (enable)
		wib->enable_in_progress = true;
	spin_unlock_irqrestore(&wib->lock, flags);

	if (enable) {
		/*
		 * The feature flag was set in the in-memory superblock with
		 * the request and is written by this commit; the log must be
		 * durable before that, so a failure has to fail the commit.
		 */
		ret = btrfs_wib_enable(fs_info);
		spin_lock_irqsave(&wib->lock, flags);
		wib->enable_in_progress = false;
		disable = wib->disable_requested;
		spin_unlock_irqrestore(&wib->lock, flags);
		if (ret)
			return ret;
		/*
		 * A disable that arrived while the log was being written out
		 * already cleared the flag from the in-memory superblock.
		 * Setting it again here would make this commit persist a
		 * feature the administrator was told had been turned off; the
		 * log stays enabled for one commit and the disable completes
		 * at the next one, as it does in the ordinary case.
		 */
		if (!disable)
			btrfs_set_fs_compat_ro(fs_info, RAID56_WRITE_INTENT);
		return 0;
	}
	if (!enabled)
		return 0;

	if (disable) {
		/*
		 * Keep persisting until a superblock without the flag has been
		 * written: this commit's superblock lacks it, the next commit
		 * knows it is durable.
		 */
		const bool flag_written = btrfs_super_compat_ro_flags(fs_info->super_for_commit) &
					  BTRFS_FEATURE_COMPAT_RO_RAID56_WRITE_INTENT;

		spin_lock_irqsave(&wib->lock, flags);
		if (!flag_written && wib->disable_armed) {
			wib->enabled = false;
			wib->disable_requested = false;
			wib->disable_armed = false;
			spin_unlock_irqrestore(&wib->lock, flags);
			/* The last block: see wib_write_final_locked(). */
			mutex_lock(&wib->commit_mutex);
			ret = wib_write_final_locked(wib, flushed);
			mutex_unlock(&wib->commit_mutex);
			if (ret < 0)
				btrfs_warn(fs_info,
	"raid56 write-intent log: could not write the final log block, the next mount will scrub the stripes the previous one listed");
			if (!btrfs_is_testing(fs_info))
				btrfs_info(fs_info, "raid56 write-intent log disabled");
			return 0;
		}
		wib->disable_armed = !flag_written;
		spin_unlock_irqrestore(&wib->lock, flags);
	}

	mutex_lock(&wib->commit_mutex);
	spin_lock_irqsave(&wib->lock, flags);
	seq = ++wib->snap_seq;
	spin_unlock_irqrestore(&wib->lock, flags);
	/* Who failed the barriers, for the records a failed one keeps to name. */
	if (!flushed)
		wib_barrier_failed(fs_info, &wib->flush_failed);
	else
		memset(&wib->flush_failed, 0, sizeof(wib->flush_failed));
	timed = wib->prepared_valid;
	if (!timed) {
		/* No snapshot before the flush: don't trust it, drop nothing. */
		ret = btrfs_wib_build_block(wib, wib->prepared, 0, NULL);
		ASSERT(ret == 0);
		flushed = false;
	}
	wib->prepared_valid = false;
	ret = wib_drop_locked(wib, seq, wib->prepared, flushed, &wib->flush_failed, false,
			      timed);
	mutex_unlock(&wib->commit_mutex);
	if (ret == -ENOSPC) {
		/*
		 * Not even the union fits; the devices keep their current
		 * blocks, which list everything that may be in flight.
		 */
		btrfs_warn_rl(fs_info,
			      "raid56 write-intent log: block full, keeping the previous one");
	} else if (ret < 0) {
		/*
		 * A failed lazy commit only means that stale entries remain on
		 * the devices that didn't get the new block; recovery is a
		 * superset then, which is safe.  RMWs waiting for their own
		 * record retry the commit and fail on their own if it keeps
		 * failing.  Nothing to abort the transaction for.
		 */
		btrfs_warn_rl(fs_info,
			      "raid56 write-intent log: lazy commit failed: %d", ret);
	}
	return 0;
}

/*
 * Start persisting the log.  Everything currently in flight is written out
 * with a flush before this returns, so that a crash right after the feature
 * flag becomes durable is covered.
 */
int btrfs_wib_enable(struct btrfs_fs_info *fs_info)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	bool was_enabled;
	u64 seq;
	int ret;

	if (!wib)
		return -EOPNOTSUPP;
	if (btrfs_is_zoned(fs_info))
		return -EOPNOTSUPP;

	mutex_lock(&wib->commit_mutex);
	spin_lock_irqsave(&wib->lock, flags);
	was_enabled = wib->enabled;
	wib->enabled = true;
	wib->enable_requested = false;
	/*
	 * A write in flight may have issued its bios while nothing kept track
	 * of them (btrfs_wib_note_written()): count every member of it written.
	 */
	for (int i = 0; !was_enabled && i < BTRFS_WIB_NR_ENTRIES; i++) {
		const struct btrfs_wib_entry *e = &wib->entries[i];
		struct btrfs_wib_written *w;

		if (!e->bitmap)
			continue;
		w = wib_written_get(wib, e->bytenr);
		if (w) {
			w->cols |= e->bitmap;
			w->par |= e->bitmap;
		}
	}
	/*
	 * A readd owed since before the disable has waited for nothing while
	 * no write was recorded: its wait for the room starts now.
	 */
	if (!was_enabled && wib->readd_owed)
		wib->readd_until = jiffies + BTRFS_WIB_READD_WAIT;
	/*
	 * Only a disable this enable supersedes is cancelled.  One raised
	 * against this very enable (enable_in_progress) has to survive: the
	 * caller re-checks it and leaves the feature flag clear.
	 */
	if (!wib->enable_in_progress) {
		wib->disable_requested = false;
		wib->disable_armed = false;
	}
	spin_unlock_irqrestore(&wib->lock, flags);

	spin_lock_irqsave(&wib->lock, flags);
	seq = ++wib->snap_seq;
	spin_unlock_irqrestore(&wib->lock, flags);
	ret = wib_flush_and_drop_locked(wib, seq, true);
	if (ret < 0 && !was_enabled) {
		spin_lock_irqsave(&wib->lock, flags);
		wib->enabled = false;
		wib_written_reset_locked(wib);
		spin_unlock_irqrestore(&wib->lock, flags);
	}
	mutex_unlock(&wib->commit_mutex);

	if (ret == 0 && !was_enabled && !btrfs_is_testing(fs_info))
		btrfs_info(fs_info, "raid56 write-intent log enabled");
	return ret;
}

/*
 * Stop persisting the log once the superblock without the feature flag is
 * durable (see btrfs_wib_commit()).  Until then the log is maintained: a
 * crash before that superblock lands would be recovered with the log by a
 * kernel that sees the flag.
 */
void btrfs_wib_disable(struct btrfs_fs_info *fs_info)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;

	if (!wib)
		return;
	spin_lock_irqsave(&wib->lock, flags);
	wib->enable_requested = false;
	if (wib->enabled || wib->enable_in_progress) {
		wib->disable_requested = true;
		wib->disable_armed = false;
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	if (!btrfs_is_testing(fs_info))
		btrfs_info(fs_info,
			   "raid56 write-intent log will be disabled after the next commit");
}

/*
 * Set the feature flag (so that it goes out with this transaction's
 * superblock) and request the log to be enabled at that commit, before the
 * superblock is written.  The two become durable together: btrfs_wib_commit()
 * writes the log out first and a failure there aborts the commit, so the flag
 * never promises a log that is not there.
 *
 * @automatic is set by the filesystem enabling the log by itself (the first
 * RAID56 chunk), and clear when an administrator asked for it.
 *
 * Nothing here does device IO, because none of the callers can afford it.
 * Chunk allocation holds chunk_mutex.  A sysfs store holds the kernfs node
 * active for as long as it runs, so an enable that waited on a wedged device
 * would hold off the removal of that node -- and therefore unmount -- with no
 * way to interrupt it.  The commit path has neither problem.
 */
int btrfs_wib_request_enable(struct btrfs_fs_info *fs_info, bool automatic)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;

	if (!wib)
		return -EOPNOTSUPP;
	if (btrfs_is_zoned(fs_info))
		return -EOPNOTSUPP;
	/*
	 * noraid56_write_intent suppresses the log being turned on by the
	 * filesystem itself; it does not overrule an administrator asking for
	 * it through sysfs or the ioctl.
	 */
	if (automatic && btrfs_test_opt(fs_info, NORAID56_WRITE_INTENT))
		return -EOPNOTSUPP;

	spin_lock_irqsave(&wib->lock, flags);
	if (!wib->enabled)
		wib->enable_requested = true;
	/*
	 * A disable that has not taken effect yet is cancelled: the flag is
	 * about to be set again, which is what disable_armed waits to see
	 * gone.
	 */
	wib->disable_requested = false;
	wib->disable_armed = false;
	spin_unlock_irqrestore(&wib->lock, flags);
	btrfs_set_fs_compat_ro(fs_info, RAID56_WRITE_INTENT);
	return 0;
}

/*
 * Telling the user.
 *
 * Every way this code can fail a write, abandon a repair or lose track of a
 * stripe goes through btrfs_raid56_alert(), which makes sure a person or a
 * monitor finds out, by five routes:
 *
 * - the first time each kind of event happens in an episode, one kernel
 *   message that is not rate limited and says what happened, which device,
 *   what it means for the applications, and what to do;
 * - while events keep coming, a summary with the counts once a minute, so a
 *   flood is not reduced to "callbacks suppressed";
 * - /sys/fs/btrfs/<fsid>/raid56_health: the state (ok, degraded, failing),
 *   the counts, the devices involved and the action to take; it can be
 *   waited on with poll() and changes state with a sysfs_notify();
 * - a KOBJ_CHANGE uevent on the filesystem's sysfs directory at every state
 *   change, carrying BTRFS_RAID56_HEALTH, BTRFS_RAID56_EVENT and
 *   BTRFS_RAID56_DEVID, for udev rules and monitoring daemons;
 * - and for the applications themselves, the EIO of the write -- which the
 *   data write path also reports per file through fserror_report(), so a
 *   fanotify FAN_FS_ERROR listener sees it (see end_bbio_data_write()).
 *
 * An episode begins with the first event and ends when nothing is recorded
 * stale or waiting any more -- every affected stripe repaired, by the queued
 * repair, a write, a scrub, a replace or the next mount.  The state is
 * FAILING from any event but a plain device write error until then,
 * DEGRADED while anything is recorded, OK otherwise.
 *
 * Callable in any context, including under wib->lock with interrupts off:
 * everything that can sleep happens in @alert_work.  Lock order: wib->lock,
 * then alert_lock, never the reverse.
 */
#define BTRFS_RAID56_ALERT_PERIOD	(60 * HZ)

static const char * const raid56_event_names[BTRFS_RAID56_NR_EVENTS] = {
	[BTRFS_RAID56_EV_STALE]		= "device_write_failed",
	[BTRFS_RAID56_EV_REFUSED]	= "write_refused",
	[BTRFS_RAID56_EV_NOT_DURABLE]	= "write_refused_not_durable",
	[BTRFS_RAID56_EV_UNDECIDABLE]	= "stripe_undecidable",
	[BTRFS_RAID56_EV_FAILED]	= "write_failed",
	[BTRFS_RAID56_EV_GAVE_UP]	= "repair_gave_up",
	[BTRFS_RAID56_EV_LOG_FULL]	= "log_full",
	[BTRFS_RAID56_EV_DROPPED]	= "record_dropped",
	[BTRFS_RAID56_EV_READ_AMBIGUOUS] = "read_unverifiable",
	[BTRFS_RAID56_EV_LOG_WRITE]	= "log_write_failed",
	[BTRFS_RAID56_EV_REPAIR_DROPPED] = "repair_dropped",
	[BTRFS_RAID56_EV_READ_PARITY]	= "read_parities_disagree",
	[BTRFS_RAID56_EV_LOG_UNFLUSHED]	= "log_flush_unnamed",
	[BTRFS_RAID56_EV_REPLACE_LOST]	= "replace_uncopyable",
	[BTRFS_RAID56_EV_REPLACE_DROPPED] = "replace_record_dropped",
	[BTRFS_RAID56_EV_REPLACE_ABORTED] = "replace_aborted",
	[BTRFS_RAID56_EV_READ_UNRECOVERED] = "read_unrecovered",
	[BTRFS_RAID56_EV_TORN_UNDECIDABLE] = "torn_undecidable",
};

/*
 * An episode ends when nothing is recorded any more, which proves every
 * damaged stripe was repaired.  These events leave no record to retire: a
 * write that failed before it reached a disk, a record the log had to drop,
 * a read refused because the parities disagree, a device that failed a
 * flush and could not be named in the records it left (the records retire;
 * the name never existed), a replace failed because the log dropped one of
 * its records, or because it could not make one.  Ending the episode on "no
 * record" would turn the
 * state back to ok the moment it went failing -- no poll() wake-up and no
 * uevent anyone could see.  They hold it failing until someone acknowledges
 * them: btrfs_raid56_health_ack().
 */
#define BTRFS_RAID56_LATCHED_EVENTS	(BIT(BTRFS_RAID56_EV_DROPPED) |		\
					 BIT(BTRFS_RAID56_EV_LOG_WRITE) |	\
					 BIT(BTRFS_RAID56_EV_READ_PARITY) |	\
					 BIT(BTRFS_RAID56_EV_LOG_UNFLUSHED) |	\
					 BIT(BTRFS_RAID56_EV_REPLACE_DROPPED) |	\
					 BIT(BTRFS_RAID56_EV_REPLACE_ABORTED))

static const char * const raid56_health_names[] = {
	[BTRFS_RAID56_HEALTH_OK]	= "ok",
	[BTRFS_RAID56_HEALTH_DEGRADED]	= "degraded",
	[BTRFS_RAID56_HEALTH_FAILING]	= "failing",
};

/*
 * Testing only: explain a stripe a write may have torn as before
 * raid56_alert_scrub_decides() -- read_unrecovered blames the mount's
 * recovery and promises that a scrub makes the reads work again, which no
 * scrub does while the record names a column only the last parity can
 * rebuild; torn_undecidable and stripe_undecidable say nothing of what does
 * clear such a stripe, nor raid56_health's action -- and let a scrub that
 * leaves such a stripe untouched (SCRUB_WIB_TORN) raise no alert.  The
 * negative control for the runtime arm of uml/torn_present.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool torn_remedy_legacy;
module_param_named(raid56_wf_torn_remedy_legacy, torn_remedy_legacy, bool, 0644);
MODULE_PARM_DESC(raid56_wf_torn_remedy_legacy,
		 "Explain reads refused and stripes left undecided because a write may have torn them as before, promising a scrub clears what it cannot, and raise no alert when a scrub leaves such a stripe untouched (testing only: restores the old behaviour)");

bool btrfs_wib_torn_remedy_legacy(void)
{
	return READ_ONCE(torn_remedy_legacy);
}
#else
static const bool torn_remedy_legacy;
#endif

/*
 * Was the read of @full_stripe refused because no recovery has taken its
 * record over yet (btrfs_wib_unrecovered()), rather than because a recovery
 * kept it undecided?  A read-write remount's recovery takes the log's records
 * over a stripe at a time, and one it stops early leaves the rest untaken, so
 * the mount-wide @pending_unrecovered cannot say which one a stripe is.
 */
static bool raid56_alert_unrecovered(struct btrfs_fs_info *fs_info, u64 full_stripe)
{
	struct btrfs_chunk_map *map;
	bool ret;

	if (!READ_ONCE(fs_info->wib->pending_unrecovered))
		return false;
	map = btrfs_find_chunk_map(fs_info, full_stripe, BTRFS_WIB_BLOCK_SIZE);
	if (!map)
		return true;
	ret = !(map->type & BTRFS_BLOCK_GROUP_RAID56_MASK) ||
	      btrfs_wib_unrecovered(fs_info, full_stripe, nr_data_stripes(map)) ||
	      btrfs_wib_recovering(fs_info, full_stripe);
	btrfs_free_chunk_map(map);
	return ret;
}

/*
 * Would a scrub decide @full_stripe, a write into which may have been torn,
 * once every device is back?  Only if the record names fewer of its data
 * columns stale than it leaves parities to rebuild them with: a rebuild that
 * spends every parity left has nothing to check it with, and
 * scrub_raid56_plan_wib() leaves such a stripe as it is (SCRUB_WIB_TORN,
 * SCRUB_WIB_AMBIGUOUS).  With none named it rewrites the parity from the
 * data.  Not under wib->lock.
 */
static bool raid56_alert_scrub_decides(struct btrfs_fs_info *fs_info, u64 full_stripe)
{
	struct btrfs_wib_stripe_state st;
	struct btrfs_chunk_map *map;
	int nr_data;
	int nr_parity;

	map = btrfs_find_chunk_map(fs_info, full_stripe, BTRFS_WIB_BLOCK_SIZE);
	if (!map)
		return true;
	nr_data = nr_data_stripes(map);
	nr_parity = map->num_stripes - nr_data;
	btrfs_free_chunk_map(map);
	if (nr_data > 64 || nr_parity > 2 ||
	    !btrfs_wib_stripe_state(fs_info, full_stripe, nr_data, nr_parity, &st) ||
	    !st.stale_cols)
		return true;
	return hweight64(st.stale_cols) < nr_parity - hweight32(st.bad_parity);
}

/* The one message a person must not miss, once per kind per episode. */
static void raid56_alert_explain(struct btrfs_fs_info *fs_info,
				 enum btrfs_raid56_event ev, u64 logical,
				 u64 devid, const char *name)
{
	const u8 *fsid = fs_info->fs_devices->fsid;
	char who[BTRFS_RAID56_ALERT_NAME + 32];

	if (devid)
		snprintf(who, sizeof(who), "devid %llu (%s)", devid, name);
	else
		strscpy(who, "a device", sizeof(who));

	switch (ev) {
	case BTRFS_RAID56_EV_STALE:
		btrfs_warn(fs_info,
"raid56: %s failed a write into full stripe %llu; the parity still holds the data and a repair will be attempted in the background. If the device keeps failing, the repair gives up and writes into its damaged stripes are refused: check it, and replace it with 'btrfs replace start %llu <new device> <mountpoint>'. State: /sys/fs/btrfs/%pU/raid56_health",
			   who, logical, devid, fsid);
		break;
	case BTRFS_RAID56_EV_REFUSED:
		btrfs_err(fs_info,
"raid56: REFUSED a write into full stripe %llu (EIO to the application; a refused metadata write makes the filesystem read-only): its data on %s is stale and the device did not take the repair, so writing would have put acknowledged data at risk. Writes into stripes with stale data on that device will keep failing until it works again or is replaced ('btrfs replace start %llu <new device> <mountpoint>', then 'btrfs scrub start <mountpoint>'). State: /sys/fs/btrfs/%pU/raid56_health",
			  logical, who, devid, fsid);
		break;
	case BTRFS_RAID56_EV_NOT_DURABLE:
		btrfs_err(fs_info,
"raid56: REFUSED a write into full stripe %llu (EIO to the application; a refused metadata write makes the filesystem read-only): its repair could not be made durable because a device did not confirm a cache flush. Find the failing device (btrfs device stats <mountpoint>, dmesg) and replace it, then run 'btrfs scrub start <mountpoint>'. State: /sys/fs/btrfs/%pU/raid56_health",
			  logical, fsid);
		break;
	case BTRFS_RAID56_EV_UNDECIDABLE:
		if (READ_ONCE(torn_remedy_legacy)) {
			btrfs_err(fs_info,
"raid56: full stripe %llu has more of it recorded stale or missing (%s among them) than its parity can rebuild -- a missing device, or writes that failed on more devices than the parity covers; writes into it fail with EIO and it cannot be repaired automatically. The data still on the disks is kept. Bring back any missing device and fix or replace the failing ones, then run 'btrfs scrub start <mountpoint>'. State: /sys/fs/btrfs/%pU/raid56_health",
				  logical, who, fsid);
			break;
		}
		btrfs_err(fs_info,
"raid56: full stripe %llu has more of it recorded stale or missing (%s among them) than its parity can rebuild -- a missing device, writes that failed on more devices than the parity covers, or a column recorded stale in a torn stripe (torn_undecidable); writes into it fail with EIO and it cannot be repaired automatically. The data still on the disks is kept. Bring back any missing device and fix or replace the failing ones, then run 'btrfs scrub start <mountpoint>'. If the scrub leaves it untouched with every device there, find the files that read as EIO ('btrfs inspect-internal logical-resolve %llu <mountpoint>' and the stripe's next addresses), delete them, run 'sync' and 'btrfs scrub start <mountpoint>' again, which retires the record, and restore them from a backup. State: /sys/fs/btrfs/%pU/raid56_health",
			  logical, who, logical, fsid);
		break;
	case BTRFS_RAID56_EV_FAILED:
		btrfs_err(fs_info,
"raid56: a write into full stripe %llu failed on more devices than its parity covers (%s among them); the application got EIO. Check the devices (btrfs device stats <mountpoint>). State: /sys/fs/btrfs/%pU/raid56_health",
			  logical, who, fsid);
		break;
	case BTRFS_RAID56_EV_GAVE_UP:
		btrfs_err(fs_info,
"raid56: gave up repairing full stripe %llu, %s keeps failing. It stays recorded; writes into it will be refused until the device works again or is replaced ('btrfs replace start %llu <new device> <mountpoint>', then 'btrfs scrub start <mountpoint>'). State: /sys/fs/btrfs/%pU/raid56_health",
			  logical, who, devid, fsid);
		break;
	case BTRFS_RAID56_EV_LOG_FULL:
		btrfs_err(fs_info,
"raid56: a write near %llu FAILED (EIO to the application) because the write-intent log is full of records that name stale data -- left by writes a device failed, or by a cache flush it did not confirm -- and they cannot be dropped without forgetting which copy is wrong, so the log refuses new writes instead. The records retire as their stripes are repaired: run 'btrfs scrub start <mountpoint>', which repairs every stripe it can -- the background repair takes only so many stripes, and gives up on a device that keeps failing. If a device keeps failing, no repair can land: fix or replace it first ('btrfs replace start <devid> <new device> <mountpoint>'). If the filesystem has gone read-only, detach that device and mount with -o degraded: records naming a missing device can be given up, and the replace can run. State: /sys/fs/btrfs/%pU/raid56_health",
			  logical, fsid);
		break;
	case BTRFS_RAID56_EV_DROPPED:
		btrfs_err(fs_info,
"raid56: the write-intent log was full and dropped the record of failed writes near %llu (not which device took them). Data with checksums there is still verified on every read; data without checksums may read back an older version, and a scrub cannot tell which copy is right -- do not count on it to restore them. Fix or replace the failing device, then acknowledge with 'echo ack > /sys/fs/btrfs/%pU/raid56_health'.",
			  logical, fsid);
		break;
	case BTRFS_RAID56_EV_LOG_WRITE:
		btrfs_err(fs_info,
"raid56: a write near %llu FAILED (EIO to the application) before reaching any disk, because the write-intent log could not be written: either too few devices took the log write, or the log is full of records it cannot drop while a device does not complete cache flushes. Every such write will fail until the device is fixed or replaced (btrfs device stats <mountpoint> names it; 'btrfs replace start <devid> <new device> <mountpoint>', then 'btrfs scrub start <mountpoint>'), then acknowledge with 'echo ack > /sys/fs/btrfs/%pU/raid56_health'.",
			  logical, fsid);
		break;
	case BTRFS_RAID56_EV_REPAIR_DROPPED:
		btrfs_warn(fs_info,
"raid56: too many damaged stripes to repair in the background; full stripe %llu and any others past the first %u stay without redundancy until a write reaches them, a scrub or the next mount. Run 'btrfs scrub start <mountpoint>' once the failing device is fixed or replaced. State: /sys/fs/btrfs/%pU/raid56_health",
			   logical, BTRFS_WIB_REPAIR_SLOTS, fsid);
		break;
	case BTRFS_RAID56_EV_READ_AMBIGUOUS:
		btrfs_err(fs_info,
"raid56: REFUSED a read of full stripe %llu (EIO to the application): more of it is missing or recorded stale (%s among them) than its parity can rebuild, and data without a checksum would have come back WRONG. Bring back any missing device, then run 'btrfs scrub start <mountpoint>'. State: /sys/fs/btrfs/%pU/raid56_health",
			  logical, who, fsid);
		break;
	case BTRFS_RAID56_EV_READ_PARITY:
		btrfs_err(fs_info,
"raid56: REFUSED a read of full stripe %llu (EIO to the application): data without a checksum had to be rebuilt (%s did not return it), and the stripe's two parities give different answers for it -- one of them is stale or corrupt and nothing can tell which, so either answer could be WRONG. Bring back or replace the device, run 'btrfs scrub start <mountpoint>', then acknowledge with 'echo ack > /sys/fs/btrfs/%pU/raid56_health'.",
			  logical, who, fsid);
		break;
	case BTRFS_RAID56_EV_LOG_UNFLUSHED:
		btrfs_err(fs_info,
"raid56: a device did not confirm a cache flush, so writes it acknowledged may never have reached its disk, and the write-intent log has more of those stripes than it can hold while naming that device in them. It keeps them recorded, but not which device is stale: data without checksums there may read back an older version, with no error, and a scrub cannot tell which side to trust. Find the device (btrfs device stats <mountpoint>: flush_io_errs), fix or replace it ('btrfs replace start <devid> <new device> <mountpoint>'), run 'btrfs scrub start <mountpoint>', then acknowledge with 'echo ack > /sys/fs/btrfs/%pU/raid56_health'.",
			  fsid);
		break;
	case BTRFS_RAID56_EV_READ_UNRECOVERED:
		if (raid56_alert_unrecovered(fs_info, logical))
			btrfs_err(fs_info,
"raid56: REFUSED a read of full stripe %llu (EIO to the application): the write-intent log lists a write into it that may have been torn by a crash, and this read-only mount does not recover the log, so nothing has checked the stripe's parity against its data. Data without a checksum there had to be rebuilt from that parity (%s did not return it) with none left over to check the rebuild, and could have come back WRONG. Mount the filesystem read-write (with -o degraded if a device is missing): its recovery decides the stripe. State: /sys/fs/btrfs/%pU/raid56_health",
				  logical, who, fsid);
		else if (READ_ONCE(torn_remedy_legacy))
			btrfs_err(fs_info,
"raid56: REFUSED a read of full stripe %llu (EIO to the application): a write into it may have been torn -- by a crash, or by a device that lost writes from its cache -- and the mount's recovery kept it undecided (its messages say why), so nothing has checked the stripe's parity against its data. Data without a checksum there had to be rebuilt from that parity (%s did not return it) with none left over to check the rebuild, and could have come back WRONG. Bring back any missing device, fix or replace the one that does not return the data, then run 'btrfs scrub start <mountpoint>': once every sector of the stripe reads, the scrub rewrites the parity from the data and the reads work again. State: /sys/fs/btrfs/%pU/raid56_health",
				  logical, who, fsid);
		else if (raid56_alert_scrub_decides(fs_info, logical))
			btrfs_err(fs_info,
"raid56: REFUSED a read of full stripe %llu (EIO to the application): a write into it may have been torn -- by a crash the mount's recovery could not decide (its messages say why), or while a device did not confirm a cache flush (log_flush_unnamed) -- so nothing has checked the stripe's parity against its data. Data without a checksum there had to be rebuilt from that parity (%s did not return it) with none left over to check the rebuild, and could have come back WRONG. Bring back any missing device, fix or replace the one that does not return the data, then run 'btrfs scrub start <mountpoint>': once every sector of the stripe reads, the scrub rewrites the parity from the data and the reads work again. State: /sys/fs/btrfs/%pU/raid56_health",
				  logical, who, fsid);
		else
			btrfs_err(fs_info,
"raid56: REFUSED a read of full stripe %llu (EIO to the application): a write into it may have been torn -- by a crash the recovery could not decide, or while a device did not confirm a cache flush -- and the write-intent log records stale a member only its last parity could rebuild. Data without a checksum there had to be rebuilt from that parity (%s did not return it) with none left over to check it, and could have come back WRONG; nothing can decide it, and a scrub leaves the stripe as it is. If a device is missing, bring it back and mount again. Otherwise find the files ('btrfs inspect-internal logical-resolve %llu <mountpoint>' and the stripe's next addresses), delete them, run 'sync' and 'btrfs scrub start <mountpoint>', which then retires the record, and restore them from a backup. State: /sys/fs/btrfs/%pU/raid56_health",
				  logical, who, logical, fsid);
		break;
	case BTRFS_RAID56_EV_TORN_UNDECIDABLE:
		if (READ_ONCE(torn_remedy_legacy)) {
			btrfs_err(fs_info,
"raid56: the mount's recovery could not decide full stripe %llu: a write into it may have been torn by a crash, and a data column it would have to rebuild -- on a missing device, or recorded stale -- has no parity left over to check that rebuild against. It records the stripe undecidable: its data without a checksum that needs the rebuild reads as EIO and writes into the stripe fail (if the log could not keep that, the message before this one says so, and such reads may return a wrong rebuild). The data on the disks is left as it is. If a device is missing, bring it back and mount again, which decides the stripe if anything can; what stays undecided (this message again) holds files ('btrfs inspect-internal logical-resolve %llu <mountpoint>') that cannot be read back with certainty: restore them from a backup. State: /sys/fs/btrfs/%pU/raid56_health",
				  logical, logical, fsid);
			break;
		}
		btrfs_err(fs_info,
"raid56: full stripe %llu cannot be decided: a write into it may have been torn -- by a crash, or while a device did not confirm a cache flush -- and a data column it would have to rebuild, on a missing device or recorded stale, has no parity left over to check that rebuild. Its data without a checksum that needs the rebuild reads as EIO, and the mount's recovery records it so that writes into it fail too (if the log could not keep that, the message before this one says so). If a device is missing, bring it back and mount again. What stays undecided holds files that cannot be read back with certainty: find them ('btrfs inspect-internal logical-resolve %llu <mountpoint>' and the stripe's next addresses), delete them, run 'sync' and 'btrfs scrub start <mountpoint>', which then retires the record, and restore them from a backup. State: /sys/fs/btrfs/%pU/raid56_health",
			  logical, logical, fsid);
		break;
	case BTRFS_RAID56_EV_REPLACE_LOST:
		btrfs_err(fs_info,
"raid56: a device replace could neither copy nor rebuild part of full stripe %llu for the new device: the old device did not return it (or the write-intent log records it stale there) and the rest of the stripe could not rebuild it. The new device holds zeros there and the log records that column or parity stale, so nothing is read or rebuilt from the zeros: reads that need them fail with EIO rather than return wrong data, and data there is lost unless a device that failed a read comes back ('btrfs replace status' counts the sectors, 'btrfs inspect-internal logical-resolve <logical> <mountpoint>' names the files). If the record could not be kept the replace was aborted instead. State: /sys/fs/btrfs/%pU/raid56_health",
			  logical, fsid);
		break;
	case BTRFS_RAID56_EV_REPLACE_DROPPED:
		btrfs_err(fs_info,
"raid56: the write-intent log was full with a device missing and had to drop the record of zeros a running device replace put on its new device in full stripe %llu, where it could neither copy nor rebuild the old device's data. The replace FAILS rather than let the new device serve those zeros as data: the old device stays as it was, present or missing, and nothing is lost that was not lost already. Writes into the degraded array fill the log with records that cannot retire until a replace finishes: pause them, then start the replace again ('btrfs replace start <devid> <new device> <mountpoint>'). If the machine went down before the replace stopped and it resumes at the next mount, cancel it ('btrfs replace cancel <mountpoint>') and start it again. Then acknowledge with 'echo ack > /sys/fs/btrfs/%pU/raid56_health'.",
			  logical, fsid);
		break;
	case BTRFS_RAID56_EV_REPLACE_ABORTED:
		btrfs_err(fs_info,
"raid56: a device replace was ABORTED at full stripe %llu: it could neither copy nor rebuild part of it for the new device, and could not record that -- the write-intent log is not enabled, is full, or cannot describe that stripe; the message before this one says which -- so it stopped rather than leave zeros on the new device that would read back as data. The old device stays in the filesystem, as it was, and nothing is lost that was not lost already. Mount without noraid56_write_intent if the log is off, run 'btrfs scrub start <mountpoint>' so that its records retire if it is full, and bring back or fix the device whose read failed if you can; then start the replace again ('btrfs replace start <devid> <new device> <mountpoint>') and acknowledge with 'echo ack > /sys/fs/btrfs/%pU/raid56_health'.",
			  logical, fsid);
		break;
	default:
		break;
	}
}

static void raid56_alert_note_dev(struct btrfs_wib *wib, u64 devid, const char *name)
{
	struct btrfs_raid56_alert_dev *free = NULL;

	lockdep_assert_held(&wib->alert_lock);
	for (int i = 0; i < BTRFS_RAID56_ALERT_DEVS; i++) {
		struct btrfs_raid56_alert_dev *d = &wib->alert_devs[i];

		if (d->devid == devid) {
			d->events++;
			return;
		}
		if (!d->devid && !free)
			free = d;
	}
	/* More devices than slots: the counts still say how bad it is. */
	if (free) {
		free->devid = devid;
		free->events = 1;
		strscpy(free->name, name, sizeof(free->name));
	}
}

static void raid56_alert_kick(struct btrfs_wib *wib, unsigned long delay)
{
	unsigned long flags;

	spin_lock_irqsave(&wib->alert_lock, flags);
	if (!wib->alert_stopped) {
		if (delay)
			queue_delayed_work(system_wq, &wib->alert_work, delay);
		else
			mod_delayed_work(system_wq, &wib->alert_work, 0);
	}
	spin_unlock_irqrestore(&wib->alert_lock, flags);
}

/*
 * Note @dev as involved in an alert, and as the one it names if it is the
 * first.  Caller holds alert_lock and the RCU read lock.
 */
static void raid56_alert_dev(struct btrfs_wib *wib, const struct btrfs_device *dev,
			     u64 *devid, char *name)
{
	char this[BTRFS_RAID56_ALERT_NAME];
	const char *n;

	lockdep_assert_held(&wib->alert_lock);
	n = btrfs_dev_name(dev);
	strscpy(this, n ? n : "?", sizeof(this));
	raid56_alert_note_dev(wib, dev->devid, this);
	if (!*devid) {
		*devid = dev->devid;
		strscpy(name, this, BTRFS_RAID56_ALERT_NAME);
	}
}

/*
 * @bioc and @cols name the devices involved, if known: bit i of @cols is
 * bioc->stripes[i].  Or @failed does, the devices that did not confirm a
 * flush.  Pass NULL and 0 when no device is to blame.
 */
static void raid56_alert(struct btrfs_fs_info *fs_info, enum btrfs_raid56_event ev,
			 u64 logical, const struct btrfs_io_context *bioc,
			 unsigned long cols, const struct btrfs_wib_flush_failed *failed)
{
	struct btrfs_wib *wib = fs_info->wib;
	char name[BTRFS_RAID56_ALERT_NAME] = "";
	unsigned long flags;
	u64 devid = 0;
	bool first;
	int col;

	if (!wib || WARN_ON_ONCE(ev >= BTRFS_RAID56_NR_EVENTS))
		return;
	atomic64_inc(&wib->stat_alert[ev]);
	if (btrfs_is_testing(fs_info))
		return;

	spin_lock_irqsave(&wib->alert_lock, flags);
	rcu_read_lock();
	if (bioc) {
		for_each_set_bit(col, &cols, min_t(int, bioc->num_stripes, BITS_PER_LONG)) {
			const struct btrfs_device *dev = bioc->stripes[col].dev;

			if (dev)
				raid56_alert_dev(wib, dev, &devid, name);
		}
	}
	if (failed) {
		struct btrfs_device *dev;

		list_for_each_entry_rcu(dev, &fs_info->fs_devices->devices, dev_list)
			if (wib_flush_failed_dev(failed, dev))
				raid56_alert_dev(wib, dev, &devid, name);
	}
	rcu_read_unlock();
	first = !(wib->alert_seen & BIT(ev));
	wib->alert_seen |= BIT(ev);
	wib->alert_pending[ev]++;
	/* Notices: redundancy lost, nothing failed. */
	if (ev != BTRFS_RAID56_EV_STALE && ev != BTRFS_RAID56_EV_REPAIR_DROPPED)
		wib->alert_failing = true;
	if (BIT(ev) & BTRFS_RAID56_LATCHED_EVENTS) {
		wib->alert_latched |= BIT(ev);
		wib->alert_latch_seq++;
	}
	/*
	 * A monitor hears of every new kind of trouble and of every latched
	 * event, not only of changes of state: a second device failing while
	 * the state is already failing is news too.
	 */
	if (first || (BIT(ev) & BTRFS_RAID56_LATCHED_EVENTS))
		wib->alert_announce = true;
	wib->last_event = ev;
	wib->last_logical = logical;
	wib->last_devid = devid;
	wib->last_jiffies = jiffies;
	strscpy(wib->last_name, name, sizeof(wib->last_name));
	if (!wib->alert_stopped) {
		/* The first of its kind is announced at once, the rest summed up. */
		if (first)
			mod_delayed_work(system_wq, &wib->alert_work, 0);
		else
			queue_delayed_work(system_wq, &wib->alert_work,
					   BTRFS_RAID56_ALERT_PERIOD);
	}
	spin_unlock_irqrestore(&wib->alert_lock, flags);

	if (first)
		raid56_alert_explain(fs_info, ev, logical, devid, name);
}

void btrfs_raid56_alert(struct btrfs_fs_info *fs_info, enum btrfs_raid56_event ev,
			u64 logical, const struct btrfs_io_context *bioc,
			unsigned long cols)
{
	raid56_alert(fs_info, ev, logical, bioc, cols, NULL);
}

/* An alert naming the devices that did not confirm a flush. */
static void raid56_alert_failed(struct btrfs_fs_info *fs_info, enum btrfs_raid56_event ev,
				u64 logical, const struct btrfs_wib_flush_failed *failed)
{
	raid56_alert(fs_info, ev, logical, NULL, 0, failed);
}

/*
 * Testing only: refuse a read that btrfs_wib_unrecovered() stands behind as
 * read_unverifiable, whose explanation blames a stale or missing column and
 * sends the administrator to scrub, and let raid56_health read ok while the
 * log's unrecovered records are all that is listed -- as before
 * BTRFS_RAID56_EV_READ_UNRECOVERED.  The negative control for the read-only
 * health arm of uml/degraded_crash.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool unrecovered_as_ambiguous;
module_param_named(raid56_wf_unrecovered_as_ambiguous, unrecovered_as_ambiguous, bool, 0644);
MODULE_PARM_DESC(raid56_wf_unrecovered_as_ambiguous,
		 "Report a read refused because the write-intent log is not recovered as read_unverifiable, and let raid56_health read ok while unrecovered records are all the log lists (testing only: restores the old behaviour)");

bool btrfs_wib_unrecovered_as_ambiguous(void)
{
	return READ_ONCE(unrecovered_as_ambiguous);
}
#endif

/*
 * The log read at mount lists writes no recovery has taken over, and the read
 * path refuses what they may have torn (btrfs_wib_unrecovered()): the episode
 * lasts as long as that, as it does while anything is recorded, and the thing
 * to do is a read-write mount, which recovers them.
 */
static bool raid56_health_unrecovered(struct btrfs_wib *wib)
{
	return READ_ONCE(wib->pending_unrecovered) &&
	       !btrfs_wib_unrecovered_as_ambiguous();
}

static unsigned int wib_nr_sticky(struct btrfs_wib *wib)
{
	unsigned int nr = 0;
	unsigned long flags;

	spin_lock_irqsave(&wib->lock, flags);
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++)
		nr += hweight64(wib->entries[i].sticky);
	spin_unlock_irqrestore(&wib->lock, flags);
	return nr;
}

static void raid56_alert_work(struct work_struct *work)
{
	struct btrfs_wib *wib = container_of(to_delayed_work(work), struct btrfs_wib,
					     alert_work);
	struct btrfs_fs_info *fs_info = wib->fs_info;
	struct kobject *kobj = &fs_info->fs_devices->fsid_kobj;
	u64 pending[BTRFS_RAID56_NR_EVENTS];
	enum btrfs_raid56_health health;
	enum btrfs_raid56_health old;
	enum btrfs_raid56_event last_event;
	const unsigned int nr_sticky = wib_nr_sticky(wib);
	const unsigned int nr_stale = atomic_read(&wib->nr_stale);
	const bool unrecovered = raid56_health_unrecovered(wib);
	unsigned long flags;
	u64 last_devid;
	bool announce;
	bool any = false;

	if (btrfs_is_testing(fs_info))
		return;
	/*
	 * Mounting: a change of state now could not be announced, since the
	 * directory the uevent comes from and the file poll() waits on do
	 * not exist yet.  Look again once they do.
	 */
	if (!kobj->state_in_sysfs) {
		raid56_alert_kick(wib, HZ);
		return;
	}

	spin_lock_irqsave(&wib->alert_lock, flags);
	memcpy(pending, wib->alert_pending, sizeof(pending));
	memset(wib->alert_pending, 0, sizeof(wib->alert_pending));
	for (int i = 0; i < BTRFS_RAID56_NR_EVENTS; i++)
		any |= pending[i] != 0;
	/*
	 * Nothing recorded any more: every affected stripe was repaired, and
	 * that part of the episode is over.  Forget the devices -- a replaced
	 * one carries its old devid on a new disk -- and explain the next
	 * trouble afresh.  Only events no record stands for remain, holding
	 * the state failing until someone acknowledges them.
	 */
	if (!nr_stale && !nr_sticky && !unrecovered) {
		wib->alert_seen &= wib->alert_latched;
		memset(wib->alert_devs, 0, sizeof(wib->alert_devs));
		if (!wib->alert_latched)
			wib->alert_failing = false;
	}
	announce = wib->alert_announce;
	wib->alert_announce = false;
	if (wib->alert_failing)
		health = BTRFS_RAID56_HEALTH_FAILING;
	else if (nr_stale || nr_sticky || unrecovered)
		health = BTRFS_RAID56_HEALTH_DEGRADED;
	else
		health = BTRFS_RAID56_HEALTH_OK;
	old = wib->health;
	WRITE_ONCE(wib->health, health);
	last_event = wib->last_event;
	last_devid = wib->last_devid;
	spin_unlock_irqrestore(&wib->alert_lock, flags);

	if (any)
		btrfs_warn(fs_info,
"raid56: since the last report: %llu device write failures, %llu writes refused, %llu refused as not durable, %llu undecidable, %llu failed, %llu repairs given up, %llu log-full failures, %llu records dropped, %llu unverifiable reads, %llu log write failures, %llu repairs not queued, %llu reads with disagreeing parities, %llu failed flushes the log could not name, %llu full stripes a replace could not copy, %llu replace records dropped, %llu replaces aborted, %llu reads of possibly torn stripes refused, %llu possibly torn stripes the recovery could not decide; %u stale marks and %u blocks still recorded; health %s",
			   pending[BTRFS_RAID56_EV_STALE],
			   pending[BTRFS_RAID56_EV_REFUSED],
			   pending[BTRFS_RAID56_EV_NOT_DURABLE],
			   pending[BTRFS_RAID56_EV_UNDECIDABLE],
			   pending[BTRFS_RAID56_EV_FAILED],
			   pending[BTRFS_RAID56_EV_GAVE_UP],
			   pending[BTRFS_RAID56_EV_LOG_FULL],
			   pending[BTRFS_RAID56_EV_DROPPED],
			   pending[BTRFS_RAID56_EV_READ_AMBIGUOUS],
			   pending[BTRFS_RAID56_EV_LOG_WRITE],
			   pending[BTRFS_RAID56_EV_REPAIR_DROPPED],
			   pending[BTRFS_RAID56_EV_READ_PARITY],
			   pending[BTRFS_RAID56_EV_LOG_UNFLUSHED],
			   pending[BTRFS_RAID56_EV_REPLACE_LOST],
			   pending[BTRFS_RAID56_EV_REPLACE_DROPPED],
			   pending[BTRFS_RAID56_EV_REPLACE_ABORTED],
			   pending[BTRFS_RAID56_EV_READ_UNRECOVERED],
			   pending[BTRFS_RAID56_EV_TORN_UNDECIDABLE],
			   nr_stale, nr_sticky, raid56_health_names[health]);

	if (health != old) {
		if (health == BTRFS_RAID56_HEALTH_OK)
			btrfs_info(fs_info,
		"raid56: every recorded stripe has been repaired and every alert acknowledged; health ok (was %s)",
				   raid56_health_names[old]);
		else
			btrfs_warn(fs_info, "raid56: health %s (was %s), see /sys/fs/btrfs/%pU/raid56_health",
				   raid56_health_names[health], raid56_health_names[old],
				   fs_info->fs_devices->fsid);
	}
	if (health != old || announce) {
		char env_health[48];
		char env_event[64];
		char env_devid[48];
		char *envp[] = { env_health, env_event, env_devid, NULL };

		sysfs_notify(kobj, NULL, "raid56_health");
		if (kobj->state_in_sysfs) {
			snprintf(env_health, sizeof(env_health), "BTRFS_RAID56_HEALTH=%s",
				 raid56_health_names[health]);
			snprintf(env_event, sizeof(env_event), "BTRFS_RAID56_EVENT=%s",
				 raid56_event_names[last_event]);
			snprintf(env_devid, sizeof(env_devid), "BTRFS_RAID56_DEVID=%llu",
				 last_devid);
			kobject_uevent_env(kobj, KOBJ_CHANGE, envp);
		}
	}

	/* Keep watching until the episode is over. */
	if (health != BTRFS_RAID56_HEALTH_OK || any)
		raid56_alert_kick(wib, BTRFS_RAID56_ALERT_PERIOD);
}

/* No more alerts: unmount, or a mount that failed. */
static void raid56_alert_stop(struct btrfs_wib *wib)
{
	unsigned long flags;

	spin_lock_irqsave(&wib->alert_lock, flags);
	wib->alert_stopped = true;
	spin_unlock_irqrestore(&wib->alert_lock, flags);
	cancel_delayed_work_sync(&wib->alert_work);
}

void btrfs_raid56_alert_stop(struct btrfs_fs_info *fs_info)
{
	if (fs_info->wib)
		raid56_alert_stop(fs_info->wib);
}

/*
 * /sys/fs/btrfs/<fsid>/raid56_health.  One "key value..." per line, for
 * people and scripts alike; the totals are since mount, the devices since
 * the filesystem was last healthy.
 */
ssize_t btrfs_raid56_health_show(struct btrfs_fs_info *fs_info, char *buf)
{
	struct btrfs_wib *wib = fs_info->wib;
	struct btrfs_raid56_alert_dev devs[BTRFS_RAID56_ALERT_DEVS];
	char last_name[BTRFS_RAID56_ALERT_NAME];
	enum btrfs_raid56_event last_event;
	enum btrfs_raid56_health health;
	unsigned long last_jiffies;
	unsigned long flags;
	u64 last_logical;
	u64 last_devid;
	unsigned int nr_sticky;
	unsigned int nr_stale;
	unsigned long latched;
	unsigned long seen;
	u64 latch_seq;
	bool undecidable;
	bool unrecovered;
	bool failing;
	int len = 0;

	if (!wib)
		return sysfs_emit(buf, "state unsupported\n");

	nr_sticky = wib_nr_sticky(wib);
	nr_stale = atomic_read(&wib->nr_stale);
	unrecovered = raid56_health_unrecovered(wib);
	spin_lock_irqsave(&wib->alert_lock, flags);
	failing = wib->alert_failing;
	latched = wib->alert_latched;
	seen = wib->alert_seen;
	latch_seq = wib->alert_latch_seq;
	last_event = wib->last_event;
	last_logical = wib->last_logical;
	last_devid = wib->last_devid;
	last_jiffies = wib->last_jiffies;
	strscpy(last_name, wib->last_name, sizeof(last_name));
	memcpy(devs, wib->alert_devs, sizeof(devs));
	spin_unlock_irqrestore(&wib->alert_lock, flags);

	/*
	 * Computed here too, not only read back: the state must be right the
	 * moment a monitor looks, even before the alert work has run.
	 */
	if (failing)
		health = BTRFS_RAID56_HEALTH_FAILING;
	else if (nr_stale || nr_sticky || unrecovered)
		health = BTRFS_RAID56_HEALTH_DEGRADED;
	else
		health = BTRFS_RAID56_HEALTH_OK;

	len += sysfs_emit_at(buf, len, "state %s\n", raid56_health_names[health]);
	len += sysfs_emit_at(buf, len, "stale_marks %u\nrecorded_blocks %u\n",
			     nr_stale, nr_sticky);
	for (int i = 0; i < BTRFS_RAID56_NR_EVENTS; i++)
		len += sysfs_emit_at(buf, len, "%s %llu\n", raid56_event_names[i],
			(unsigned long long)atomic64_read(&wib->stat_alert[i]));
	if (last_jiffies)
		len += sysfs_emit_at(buf, len,
			"last_event %s full_stripe %llu devid %llu device %s seconds_ago %u\n",
			raid56_event_names[last_event], last_logical, last_devid,
			last_name[0] ? last_name : "-",
			jiffies_to_msecs(jiffies - last_jiffies) / 1000);
	else
		len += sysfs_emit_at(buf, len, "last_event none\n");
	len += sysfs_emit_at(buf, len, "unacknowledged");
	if (!latched)
		len += sysfs_emit_at(buf, len, " none");
	for (int i = 0; i < BTRFS_RAID56_NR_EVENTS; i++)
		if (latched & BIT(i))
			len += sysfs_emit_at(buf, len, " %s", raid56_event_names[i]);
	len += sysfs_emit_at(buf, len, "\n");
	len += sysfs_emit_at(buf, len, "unacknowledged_seq %llu\n", latch_seq);
	len += sysfs_emit_at(buf, len, "devices");
	for (int i = 0; i < BTRFS_RAID56_ALERT_DEVS; i++)
		if (devs[i].devid)
			len += sysfs_emit_at(buf, len, " %llu:%s:%llu", devs[i].devid,
					     devs[i].name, devs[i].events);
	len += sysfs_emit_at(buf, len, "\n");

	/*
	 * What to do.  A device named in a failing episode is to be fixed or
	 * replaced; everything else a scrub puts right.  Neither can happen
	 * before a read-write mount has recovered what the log lists, and
	 * nothing but that recovery can decide it.  Except a stripe nothing
	 * can decide with every device there: a scrub leaves it as it is while
	 * it holds data without a checksum that only a rebuild nobody can
	 * check would give back, so that goes first -- deleted, to be restored
	 * from a backup once the scrub has retired the record.
	 */
	undecidable = (seen & (BIT(BTRFS_RAID56_EV_UNDECIDABLE) |
			       BIT(BTRFS_RAID56_EV_TORN_UNDECIDABLE))) &&
		      !READ_ONCE(fs_info->fs_devices->missing_devices) &&
		      !READ_ONCE(torn_remedy_legacy);
	len += sysfs_emit_at(buf, len, "action ");
	if (unrecovered) {
		len += sysfs_emit_at(buf, len, "mount-rw\n");
	} else if (health == BTRFS_RAID56_HEALTH_FAILING) {
		bool named = false;

		for (int i = 0; i < BTRFS_RAID56_ALERT_DEVS; i++) {
			if (!devs[i].devid)
				continue;
			len += sysfs_emit_at(buf, len, "%sreplace-devid-%llu",
					     named ? "," : "", devs[i].devid);
			named = true;
		}
		len += sysfs_emit_at(buf, len, "%s%sscrub%s%s\n", named ? " then " : "",
				     undecidable ? "delete-undecidable-files then " : "",
				     undecidable ? " then restore-files" : "",
				     latched ? " then ack" : "");
	} else if (health == BTRFS_RAID56_HEALTH_DEGRADED) {
		/* Once the queue overflowed, waiting will not finish the job. */
		len += sysfs_emit_at(buf, len, "%s\n",
				     atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_REPAIR_DROPPED]) ?
				     "scrub" : "wait-for-repair-or-scrub");
	} else {
		len += sysfs_emit_at(buf, len, "none\n");
	}
	return len;
}

/*
 * Someone has seen the events that no record stands for and dealt with them:
 * "echo ack > /sys/fs/btrfs/<fsid>/raid56_health".  The state goes back to
 * what the records say, announced as any change is.
 *
 * A script should write "ack <seq>" with the unacknowledged_seq it read: if
 * another latched event arrived since, nothing is cleared and -EAGAIN tells it
 * to look again, so an ack never covers an event nobody saw.
 */
int btrfs_raid56_health_ack(struct btrfs_fs_info *fs_info, bool check_seq, u64 seq)
{
	struct btrfs_wib *wib = fs_info->wib;
	unsigned long flags;
	unsigned long was;

	if (!wib)
		return -EOPNOTSUPP;
	spin_lock_irqsave(&wib->alert_lock, flags);
	if (check_seq && seq != wib->alert_latch_seq) {
		spin_unlock_irqrestore(&wib->alert_lock, flags);
		return -EAGAIN;
	}
	was = wib->alert_latched;
	wib->alert_latched = 0;
	/* If it happens again, it is explained again. */
	wib->alert_seen &= ~was;
	spin_unlock_irqrestore(&wib->alert_lock, flags);
	if (was)
		btrfs_info(fs_info, "raid56: alerts acknowledged");
	raid56_alert_kick(wib, 0);
	return 0;
}

int btrfs_wib_alloc(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib;

	ASSERT(!fs_info->wib);

	wib = kzalloc_obj(*wib, GFP_KERNEL);
	if (!wib)
		return -ENOMEM;
	wib->block = (void *)get_zeroed_page(GFP_KERNEL);
	wib->last = (void *)get_zeroed_page(GFP_KERNEL);
	wib->prepared = (void *)get_zeroed_page(GFP_KERNEL);
	wib->flushsnap = (void *)get_zeroed_page(GFP_KERNEL);
	wib->readd_base = (void *)get_zeroed_page(GFP_KERNEL);
	wib->written = kcalloc(BTRFS_WIB_WRITTEN_SLOTS, sizeof(*wib->written), GFP_KERNEL);
	wib->snapbits = kcalloc(BTRFS_WIB_NR_ENTRIES, sizeof(*wib->snapbits), GFP_KERNEL);
	wib->readd_last = kcalloc(BTRFS_WIB_NR_ENTRIES, sizeof(*wib->readd_last), GFP_KERNEL);
	if (!wib->block || !wib->last || !wib->prepared || !wib->flushsnap ||
	    !wib->readd_base || !wib->written || !wib->snapbits || !wib->readd_last) {
		free_page((unsigned long)wib->block);
		free_page((unsigned long)wib->last);
		free_page((unsigned long)wib->prepared);
		free_page((unsigned long)wib->flushsnap);
		free_page((unsigned long)wib->readd_base);
		kfree(wib->written);
		kfree(wib->snapbits);
		kfree(wib->readd_last);
		kfree(wib);
		return -ENOMEM;
	}
	wib->fs_info = fs_info;
	spin_lock_init(&wib->lock);
	mutex_init(&wib->commit_mutex);
	init_waitqueue_head(&wib->wait);
	init_waitqueue_head(&wib->io_wait);
	ratelimit_state_init(&wib->readd_say_rs, BTRFS_WIB_READD_SAY_EVERY, 1);
	ratelimit_set_flags(&wib->readd_say_rs, RATELIMIT_MSG_ON_RELEASE);
	atomic_set(&wib->io_pending, 0);
	spin_lock_init(&wib->repair_lock);
	INIT_DELAYED_WORK(&wib->repair_work, btrfs_raid56_repair_work);
	atomic_set(&wib->repairs_inflight, 0);
	spin_lock_init(&wib->alert_lock);
	INIT_DELAYED_WORK(&wib->alert_work, raid56_alert_work);
	wib->health = BTRFS_RAID56_HEALTH_OK;
	fs_info->wib = wib;
	/*
	 * Once mounted, say whether the log came back with anything recorded
	 * from last time: that is a degraded filesystem nobody has been told
	 * about yet.
	 */
	if (!btrfs_is_testing(fs_info))
		queue_delayed_work(system_wq, &wib->alert_work, HZ);
	return 0;
}

void btrfs_wib_free(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;

	btrfs_raid56_evidence_disarm(fs_info);
	if (!wib)
		return;
	/* Normally done already by close_ctree(); a failed mount may not have. */
	btrfs_raid56_stop_repairs(fs_info);
	raid56_alert_stop(wib);
	fs_info->wib = NULL;
	free_page((unsigned long)wib->block);
	free_page((unsigned long)wib->last);
	free_page((unsigned long)wib->prepared);
	free_page((unsigned long)wib->flushsnap);
	free_page((unsigned long)wib->readd_base);
	kfree(wib->written);
	kfree(wib->snapbits);
	kfree(wib->readd_last);
	kvfree(wib->pending);
	kvfree(wib->pending_taken);
	kfree(wib);
}

/* Append a region to the pending recovery list (merged later). */
int btrfs_wib_add_pending(struct btrfs_wib *wib,
			  const struct btrfs_wib_entry *src)
{
	if (!src->bitmap && !src->sticky)
		return 0;
	if (wib->nr_pending == wib->max_pending) {
		unsigned int new_max = wib->max_pending ? wib->max_pending * 2 : 256;
		struct btrfs_wib_entry *p;

		p = kvmalloc_array(new_max, sizeof(*p), GFP_KERNEL);
		if (!p)
			return -ENOMEM;
		if (wib->pending)
			memcpy(p, wib->pending, wib->nr_pending * sizeof(*p));
		kvfree(wib->pending);
		wib->pending = p;
		wib->max_pending = new_max;
	}
	wib->pending[wib->nr_pending] = *src;
	wib->nr_pending++;
	return 0;
}

static int wib_pending_cmp(const void *a, const void *b)
{
	const struct btrfs_wib_entry *ea = a;
	const struct btrfs_wib_entry *eb = b;

	if (ea->bytenr < eb->bytenr)
		return -1;
	if (ea->bytenr > eb->bytenr)
		return 1;
	return 0;
}

/* Sort the pending list and merge entries of the same region. */
void btrfs_wib_finalize_pending(struct btrfs_wib *wib)
{
	unsigned int out = 0;

	if (wib->nr_pending == 0)
		return;
	sort(wib->pending, wib->nr_pending, sizeof(*wib->pending), wib_pending_cmp, NULL);
	for (unsigned int i = 0; i < wib->nr_pending; i++) {
		if (out && wib->pending[out - 1].bytenr == wib->pending[i].bytenr) {
			wib->pending[out - 1].bitmap |= wib->pending[i].bitmap;
			wib->pending[out - 1].sticky |= wib->pending[i].sticky;
			/*
			 * The stale record has to be unioned here like the
			 * others.  A region can be named by more than one
			 * device's newest block, and a device that took a
			 * write error is exactly the one whose block is most
					 * likely to carry the stale bits -- if the merge
			 * kept only whichever entry happened to sort first,
			 * the distinction between "the data is wrong" and
			 * "something went wrong" would be lost precisely when
			 * it matters.
			 */
			wib->pending[out - 1].stale |= wib->pending[i].stale;
			wib->pending[out - 1].stale_par |= wib->pending[i].stale_par;
			wib->pending[out - 1].torn |= wib->pending[i].torn;
			wib->pending[out - 1].gen = max(wib->pending[out - 1].gen,
							wib->pending[i].gen);
			continue;
		}
		wib->pending[out++] = wib->pending[i];
	}
	wib->nr_pending = out;
}

static int wib_read_slot(struct btrfs_device *device, unsigned int slot, void *buf)
{
	struct bio *bio;
	int ret;

	bio = bio_alloc(device->bdev, 1, REQ_OP_READ | REQ_META | REQ_PRIO, GFP_KERNEL);
	bio->bi_iter.bi_sector = (BTRFS_WIB_OFFSET + slot * BTRFS_WIB_SLOT_SIZE) >>
				 SECTOR_SHIFT;
	__bio_add_page(bio, virt_to_page(buf), BTRFS_WIB_SLOT_SIZE, offset_in_page(buf));
	ret = submit_bio_wait(bio);
	bio_put(bio);
	return ret;
}

/* Testing only: take stale marks from every device's newest block again. */
static bool load_unions_stale;
#ifdef CONFIG_BTRFS_DEBUG
module_param_named(raid56_load_unions_stale, load_unions_stale, bool, 0644);
MODULE_PARM_DESC(raid56_load_unions_stale,
		 "At mount, take stale marks from every device's newest log block, not only the newest overall (testing only: restores a known defect)");
#endif

/*
 * Add the entries of @block, the newest valid block of one device, to the
 * pending set.  @newest: it is the newest block of all, whose stale marks
 * count (see btrfs_wib_load()).
 *
 * A block whose writer did not mark the records that may hide a torn write
 * (btrfs_wib_block_marks_torn()) -- a kernel from before the mark, which
 * turned the writes a failed flush may have torn into plain error records --
 * cannot say which of its error records is one.  Every one of them is loaded
 * as possibly torn then (@torn in struct btrfs_wib_entry), the rules for a
 * write that may have been torn apply to it as they did to every record
 * before the mark existed, and a recovery that keeps it keeps it marked
 * (wib_keep_torn()), so the blocks this kernel writes from then on say so.
 * @nr_torn receives how many blocks were loaded that way.
 */
int btrfs_wib_load_block(struct btrfs_wib *wib, const void *block, bool newest,
			 unsigned int *nr_torn)
{
	const struct btrfs_wib_disk_header *hdr = block;
	const bool marks = btrfs_wib_block_marks_torn(block) ||
			   READ_ONCE(trust_unmarked_log);
	const u32 nr = le32_to_cpu(hdr->nr_entries);

	*nr_torn = 0;
	for (u32 i = 0; i < nr; i++) {
		struct btrfs_wib_entry e;
		int ret;

		btrfs_wib_read_entry(block, i, &e);
		if (!newest) {
			e.stale = 0;
			e.stale_par = 0;
		}
		if (!marks) {
			e.torn = e.sticky;
			*nr_torn += hweight64(e.torn);
		}
		ret = btrfs_wib_add_pending(wib, &e);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/*
 * Read the log blocks of all present devices and build the list of full
 * stripes to recover before the filesystem is written to.  Called at mount
 * before any write happens.
 *
 * Per device only the newest valid block counts: a block with sequence
 * number N on a device was written with a PREFLUSH to that device whenever
 * it dropped a stripe listed by N-1, so everything the older block lists
 * and the newer one doesn't is on stable media on that device.  Across
 * devices the union is taken: a device that missed a commit (torn write,
 * IO error, or absent at the time) still lists the stripes that commit
 * dropped, and scrubbing a consistent stripe is harmless.
 *
 * Except the stale marks.  "In flight" makes recovery recompute the parity
 * from the data, which is harmless on a consistent stripe; "stale" makes it
 * rebuild the named column FROM the parity, which is harmless only while the
 * parity has not moved on.  A mark the newest block no longer carries was
 * cleared on purpose -- the column was written back with FUA and the log
 * made durable before the next write changed the parity (rmw_repair_first())
 * -- and a device that missed that block still names it.  Brought back by
 * the union, it has recovery rebuild a column that is right from a parity a
 * crash has since torn: silent, for data without a checksum.  So stale and
 * stale_par come only from devices holding the newest block; every other
 * device's entries still count, without them.
 */
int btrfs_wib_load(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	struct btrfs_fs_devices *fs_devices = fs_info->fs_devices;
	struct btrfs_device *device;
	u64 max_seq = 0;
	unsigned int nr_valid = 0;
	unsigned int nr_unmarked = 0;
	unsigned int nofs_flag;
	void *buf;
	int ret = 0;

	if (!wib)
		return 0;
	/* Called before the zoned mode is set up, so ask the superblock too. */
	if (btrfs_is_zoned(fs_info) || btrfs_fs_incompat(fs_info, ZONED))
		return 0;

	buf = (void *)get_zeroed_page(GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Allocations under device_list_mutex must not enter reclaim. */
	nofs_flag = memalloc_nofs_save();
	mutex_lock(&fs_devices->device_list_mutex);

	/* The newest sequence number on any device: see above. */
	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		const struct btrfs_wib_disk_header *hdr = buf;

		if (!device->bdev ||
		    !test_bit(BTRFS_DEV_STATE_IN_FS_METADATA, &device->dev_state))
			continue;
		for (unsigned int slot = 0; slot < BTRFS_WIB_NR_SLOTS; slot++) {
			if (wib_read_slot(device, slot, buf) < 0 ||
			    !btrfs_wib_block_valid(fs_info, buf))
				continue;
			max_seq = max(max_seq, le64_to_cpu(hdr->seq));
		}
	}

	list_for_each_entry(device, &fs_devices->devices, dev_list) {
		const struct btrfs_wib_disk_header *hdr = buf;
		u64 dev_seq = 0;
		int dev_slot = -1;
		unsigned int nr;
		bool newest;

		device->wib_next_slot = 0;
		if (!device->bdev)
			continue;
		if (!test_bit(BTRFS_DEV_STATE_IN_FS_METADATA, &device->dev_state))
			continue;

		/* Find the newest valid block of this device. */
		for (unsigned int slot = 0; slot < BTRFS_WIB_NR_SLOTS; slot++) {
			ret = wib_read_slot(device, slot, buf);
			if (ret < 0) {
				btrfs_warn(fs_info,
			"raid56 write-intent log: failed to read slot %u of %s: %d",
					   slot, btrfs_dev_name(device), ret);
				btrfs_dev_stat_inc_and_print(device, BTRFS_DEV_STAT_READ_ERRS);
				ret = 0;
				continue;
			}
			if (!btrfs_wib_block_valid(fs_info, buf))
				continue;
			nr_valid++;
			if (dev_slot < 0 || le64_to_cpu(hdr->seq) > dev_seq) {
				dev_seq = le64_to_cpu(hdr->seq);
				dev_slot = slot;
			}
		}
		if (dev_slot < 0)
			continue;
		/* Don't overwrite the newest block of this device first. */
		device->wib_next_slot = (dev_slot + 1) % BTRFS_WIB_NR_SLOTS;
		newest = dev_seq >= max_seq || READ_ONCE(load_unions_stale);
		if (dev_seq > max_seq)
			max_seq = dev_seq;

		ret = wib_read_slot(device, dev_slot, buf);
		if (ret < 0 || !btrfs_wib_block_valid(fs_info, buf)) {
			/* Read it fine a moment ago, treat as an IO error. */
			btrfs_warn(fs_info,
			"raid56 write-intent log: failed to re-read slot %d of %s",
				   dev_slot, btrfs_dev_name(device));
			ret = 0;
			continue;
		}
		ret = btrfs_wib_load_block(wib, buf, newest, &nr);
		if (ret < 0)
			goto out;
		nr_unmarked += nr;
	}
out:
	mutex_unlock(&fs_devices->device_list_mutex);
	memalloc_nofs_restore(nofs_flag);
	free_page((unsigned long)buf);
	if (ret < 0)
		return ret;

	btrfs_wib_finalize_pending(wib);

	/* Continue the sequence. */
	wib->seq = max_seq;
	wib->snap_seq = max_seq;

	/*
	 * Answer reads from the record from here on, not only once a
	 * read-write recovery has run.  The tree roots are read right after
	 * this, and on a RAID6 with one device missing and a column of another
	 * left stale, those reads are two erasures if the record is consulted
	 * and an erasure plus an unlocated error -- beyond RAID6 -- if not: the
	 * mount failed on a filesystem that had everything it needed.  A
	 * read-only mount never runs the recovery at all, so without this it
	 * never consulted the record for anything.
	 */
	if (!READ_ONCE(btrfs_raid56_no_early_record)) {
		unsigned int nr = 0;
		unsigned long flags;

		for (unsigned int i = 0; i < wib->nr_pending; i++)
			nr += hweight64(wib->pending[i].stale) +
			      hweight64(wib->pending[i].stale_par);
		if (nr) {
			spin_lock_irqsave(&wib->lock, flags);
			wib->consult_pending = true;
			wib->nr_pending_stale = nr;
			atomic_add(nr, &wib->nr_stale);
			spin_unlock_irqrestore(&wib->lock, flags);
		}
	}

	if (wib->nr_pending) {
		unsigned int nr_blocks = 0;
		unsigned int nr_error = 0;
		unsigned int nr_torn = 0;
		unsigned long flags;

		for (unsigned int i = 0; i < wib->nr_pending; i++) {
			nr_blocks += hweight64(wib->pending[i].bitmap | wib->pending[i].sticky);
			nr_error += hweight64(wib->pending[i].sticky);
			nr_torn += hweight64(wib->pending[i].torn & ~wib->pending[i].bitmap);
		}
		btrfs_info(fs_info,
	"raid56 write-intent log: %u valid blocks found, %u regions with %u dirty stripes to recover (%u with earlier errors)",
			   nr_valid, wib->nr_pending, nr_blocks, nr_error);
		/*
		 * They cost reads the records of a plain failed write would not
		 * (btrfs_wib_unrecovered(), scrub_raid56_recover_absent()): say
		 * why, once.
		 */
		if (nr_unmarked && nr_torn && !READ_ONCE(all_records_torn))
			btrfs_warn(fs_info,
	"raid56 write-intent log: written by a kernel that does not mark the writes a failed flush may have torn; treating its %u error records as possibly torn until a read-write mount recovers them",
				   nr_torn);
		/*
		 * Until a recovery takes them over: see btrfs_wib_unrecovered().
		 * Part of consulting the record from the first read, so the
		 * knob that restores the time before that turns it off too.
		 */
		if (!READ_ONCE(btrfs_raid56_no_early_record)) {
			spin_lock_irqsave(&wib->lock, flags);
			WRITE_ONCE(wib->pending_unrecovered, true);
			spin_unlock_irqrestore(&wib->lock, flags);
		}
	}
	return 0;
}

/* True if any pending error record covers [@start, @start + @len). */
static bool wib_pending_has_error(struct btrfs_wib *wib, u64 start, u64 len)
{
	for (unsigned int i = 0; i < wib->nr_pending; i++) {
		const struct btrfs_wib_entry *e = &wib->pending[i];

		if (e->sticky & btrfs_wib_range_mask(e->bytenr, start, len))
			return true;
	}
	return false;
}

/*
 * True if the log read at mount lists a write into [@start, @start + @len) that
 * may have been torn: one in flight at the crash, or one marked possibly torn
 * before it, which is written as in flight (@torn in struct btrfs_wib_entry),
 * or any error record of a block whose writer did not mark
 * (btrfs_wib_load_block()).
 */
static bool wib_pending_in_flight(struct btrfs_wib *wib, u64 start, u64 len)
{
	for (unsigned int i = 0; i < wib->nr_pending; i++) {
		const struct btrfs_wib_entry *e = &wib->pending[i];

		if ((e->bitmap | e->torn) & btrfs_wib_range_mask(e->bytenr, start, len))
			return true;
	}
	return false;
}

/*
 * Put back the stale record the recovered log carried for a stripe that stays
 * recorded.
 *
 * btrfs_wib_add_sticky() restores "something went wrong here"; this restores
 * WHICH SIDE went wrong, which is the part a later scrub needs and the part
 * that used to be lost at every mount.  Without it, persisting the record on
 * disk would buy nothing: the bits would be read off the device and then
 * dropped on the floor before anything could consult them.
 *
 * Call after btrfs_wib_add_sticky() for the same range -- btrfs_wib_mark_stale()
 * only marks blocks the live table already records, on purpose.
 */
static void wib_readd_stale(struct btrfs_fs_info *fs_info, u64 start, u64 len)
{
	struct btrfs_wib *wib = fs_info->wib;

	for (unsigned int i = 0; i < wib->nr_pending; i++) {
		const struct btrfs_wib_entry *e = &wib->pending[i];
		const u64 mask = btrfs_wib_range_mask(e->bytenr, start, len);
		u64 stale = e->stale & mask;

		while (stale) {
			const unsigned int bit = __ffs64(stale);

			stale &= ~BIT_ULL(bit);
			btrfs_wib_mark_stale(fs_info,
					     e->bytenr + ((u64)bit << BTRFS_WIB_BLOCK_SHIFT),
					     BTRFS_WIB_BLOCK_SIZE);
		}
		for (int p = 0; p < 2; p++) {
			const u64 logical = start + ((u64)p << BTRFS_WIB_BLOCK_SHIFT);

			if (e->stale_par & btrfs_wib_range_mask(e->bytenr, logical,
								BTRFS_WIB_BLOCK_SIZE))
				btrfs_wib_update_stale_parity(fs_info, start, p, true);
		}
		/*
		 * And when it was made: re-adding it stamped the live entry
		 * with this mount's generation, which is not when the write
		 * failed.
		 */
		if (e->sticky & mask) {
			unsigned long flags;
			struct btrfs_wib_entry *live;

			spin_lock_irqsave(&wib->lock, flags);
			live = wib_find_entry(wib, e->bytenr);
			if (live && e->gen)
				live->gen = e->gen;
			spin_unlock_irqrestore(&wib->lock, flags);
		}
	}
}

struct wib_recovery_stats {
	unsigned int done;
	unsigned int skipped;
	unsigned int failed;
	unsigned int kept;
};

/*
 * Scrub the full stripe containing @logical, in @mode (see
 * enum btrfs_raid56_recover_mode).
 *
 * Return 0 if the stripe is consistent again and its record can go, 1 if it
 * must stay recorded, a negative error on a fatal error.  @start and @len
 * receive the full stripe geometry (@len is 0 if there is no such stripe
 * anymore).
 *
 * In RECOVER_SCRUB mode the record must already be in the live table (the
 * scrub decides from it); the caller retires it on 0.
 */
#ifdef CONFIG_BTRFS_DEBUG
/*
 * Milliseconds to linger per recovered full stripe.  Widens the window in
 * which the recovery is inside the scrub code, so a test can land a
 * transaction commit in it; see tools/testing/btrfs/uml/pausehang.sh.
 */
static int btrfs_raid56_recovery_delay_ms;
module_param_named(raid56_recovery_delay_ms, btrfs_raid56_recovery_delay_ms, int, 0644);
MODULE_PARM_DESC(raid56_recovery_delay_ms,
		 "Linger this many ms per recovered full stripe (testing only)");
#endif

/*
 * Recover error records the way this code did before it could repair them:
 * verify the data, write no parity, keep the record.  The negative control for
 * tools/testing/btrfs/uml/recover_scrub.sh.
 */
static bool btrfs_raid56_recover_legacy;
#ifdef CONFIG_BTRFS_DEBUG
module_param_named(raid56_recover_legacy, btrfs_raid56_recover_legacy, bool, 0644);
MODULE_PARM_DESC(raid56_recover_legacy,
		 "Only verify stripes with a write-failure record at mount, never repair them (testing only)");
#endif

static enum btrfs_raid56_recover_mode wib_error_mode(void)
{
	return READ_ONCE(btrfs_raid56_recover_legacy) ?
	       BTRFS_RAID56_RECOVER_VERIFY : BTRFS_RAID56_RECOVER_SCRUB;
}

static int wib_recover_one(struct btrfs_fs_info *fs_info, struct scrub_ctx *sctx,
			   u64 logical, enum btrfs_raid56_recover_mode mode,
			   bool log_replay_pending, u64 *start, u64 *len,
			   unsigned int *unwritten_par, struct wib_recovery_stats *st)
{
	struct btrfs_wib *wib = fs_info->wib;
	bool torn;
	int ret;

	*unwritten_par = 0;
	ret = btrfs_raid56_full_stripe_range(fs_info, logical, start, len);
	if (ret == -ENOENT) {
		/* Chunk gone or not RAID56 anymore, nothing to do. */
		*len = 0;
		st->skipped++;
		return 0;
	}
	if (ret < 0)
		return ret;
	/*
	 * May a write into the stripe have been torn?  The log read at mount
	 * lists one in flight, or an error record from a writer that did not
	 * mark (btrfs_wib_load_block()), or the live table marks a record an
	 * earlier pass of this mount kept (wib_keep_torn()).  Only then do the
	 * rules for a stripe with a data column on a missing device apply; a
	 * record of a plain failed write names what the failure left stale,
	 * and is decided from that like any other.
	 */
	torn = READ_ONCE(all_records_torn) || wib_pending_in_flight(wib, *start, *len) ||
	       btrfs_wib_stripe_torn(fs_info, *start, *len);

#ifdef CONFIG_BTRFS_DEBUG
	if (unlikely(READ_ONCE(btrfs_raid56_recovery_delay_ms) > 0)) {
		const int ms = READ_ONCE(btrfs_raid56_recovery_delay_ms);
		int i;

		/*
		 * Sample the pause protocol while lingering, so a test can see
		 * whether a pauser ever overlaps the recovery at all rather
		 * than inferring it from whether something hung.
		 */
		for (i = 0; i < ms; i += 100) {
			btrfs_info(fs_info,
	"raid56 recovery delay: pause_req %d paused %d running %d full stripe %llu",
				   atomic_read(&fs_info->scrub_pause_req),
				   atomic_read(&fs_info->scrubs_paused),
				   atomic_read(&fs_info->scrubs_running), *start);
			msleep(100);
		}
	}
#endif
	ret = btrfs_scrub_raid56_full_stripe(fs_info, sctx, *start, mode,
					     log_replay_pending, torn, unwritten_par);
	if (ret == -ENOENT) {
		st->skipped++;
		return 0;
	}
	/*
	 * A write into the stripe may have been torn -- at the crash, or
	 * before it while recorded -- while a device holding one of its data
	 * columns was missing, or while the record names a column stale that
	 * the parities left can rebuild only by spending every one of them,
	 * and the parity cannot say what that column holds.  The scrub
	 * recorded every parity still there as stale, which makes reads of the
	 * column's unchecksummed sectors fail (read_unverifiable) rather than
	 * return a rebuild nobody can check, and writes into the stripe fail
	 * as undecidable, and it raised torn_undecidable.  Keep the record:
	 * once a missing device is back the stripe may be decided again.
	 */
	if (ret == 4) {
		btrfs_warn(fs_info,
	"raid56 write-intent log: full stripe at %llu is recorded as possibly torn while a data column it would have to rebuild is on a missing device or recorded stale, with no parity left over to check the rebuild; that column's data without a checksum cannot be rebuilt with certainty and reads as EIO, and writes into the stripe fail, keeping it recorded",
			   *start);
		st->failed++;
		st->kept++;
		atomic64_inc(&wib->stat_recovery_suspect);
		return 1;
	}
	/* The scrub declined or could not finish; it said why. */
	if (ret == 3) {
		btrfs_warn(fs_info,
	"raid56 write-intent log: full stripe at %llu has a write-failure record that could not be resolved safely now, keeping it recorded",
			   *start);
		st->kept++;
		return 1;
	}
	if (ret == -EIO) {
		btrfs_err(fs_info,
	"raid56 write-intent log: full stripe at %llu has unrepairable sectors, keeping it recorded",
			  *start);
		st->failed++;
		st->kept++;
		atomic64_inc(&wib->stat_recovery_errors);
		return 1;
	}
	if (ret < 0) {
		btrfs_err(fs_info,
	"raid56 write-intent log: failed to recover full stripe at %llu: %d, keeping it recorded",
			  *start, ret);
		st->failed++;
		st->kept++;
		atomic64_inc(&wib->stat_recovery_errors);
		/* Only a resource shortage is worth failing the mount for. */
		if (ret == -ENOMEM)
			return ret;
		return 1;
	}
	st->done++;
	atomic64_inc(&wib->stat_recovered_stripes);
	/*
	 * A device is missing: its sectors of this stripe could be stale
	 * and were not repaired.  Keep the stripe recorded so that it is
	 * scrubbed again at the first mount with the device back (or
	 * replaced).  If that is only a parity, @unwritten_par says which,
	 * or that the data column there was decided.
	 */
	if (ret == 1) {
		st->kept++;
		return 1;
	}
	/*
	 * An unreadable sector that holds no extent (as far as the extent
	 * tree knows) prevents recomputing the parity of its vertical
	 * stripe.  Nothing referenced depends on it, unless an extent is
	 * still hidden in the tree log: then look again after the replay.
	 */
	if (ret == 2) {
		if (log_replay_pending) {
			st->kept++;
			return 1;
		}
		btrfs_warn(fs_info,
	"raid56 write-intent log: full stripe at %llu has an unreadable sector holding no extent, its parity is left alone",
			   *start);
		return 0;
	}
	/* Verified what could be, the rest waits for the log replay. */
	if (mode == BTRFS_RAID56_RECOVER_VERIFY) {
		st->kept++;
		return 1;
	}
	return 0;
}

/*
 * The recovery stopped with @err before it took every stripe over: a fatal
 * signal (a remount,rw interrupted, or timed out by whatever ran it), the
 * filesystem closing, or an error.  The mount stays read-only -- a first
 * mount fails -- and the stripes it did not reach are exactly as the log left
 * them, so @pending goes on answering for them: say so.
 */
static void wib_recovery_stopped(struct btrfs_wib *wib, int err)
{
	struct btrfs_fs_info *fs_info = wib->fs_info;
	unsigned int left = 0;
	unsigned long flags;

	if (READ_ONCE(recover_drops_refusals))
		return;
	spin_lock_irqsave(&wib->lock, flags);
	for (unsigned int i = 0; i < wib->nr_pending; i++)
		if ((wib->pending[i].bitmap | wib->pending[i].sticky) &
		    ~(wib->pending_taken ? wib->pending_taken[i] : 0))
			left++;
	spin_unlock_irqrestore(&wib->lock, flags);
	btrfs_warn(fs_info,
"raid56 write-intent log: recovery stopped (%pe) before it reached every full stripe the log lists (%u of %u region(s) not wholly recovered); the filesystem stays read-only, and for those stripes it goes on as a read-only mount does: data without a checksum that would have to be rebuilt with no parity left over to check the rebuild reads as EIO (read_unrecovered), and the columns their records name stale are not read, until a read-write mount, degraded if need be, recovers them",
		   ERR_PTR(err), left, wib->nr_pending);
}

/*
 * Recover every full stripe recorded in the log.  Must run before anything
 * is written to the filesystem (and after the block groups and the
 * extent/csum trees are available).
 *
 * @log_replay_pending: a tree log is about to be replayed, so extents only
 * it references are not visible yet.  Error records are then only verified
 * here and completed by btrfs_wib_recover_after_replay().
 */
int btrfs_wib_recover(struct btrfs_fs_info *fs_info, bool log_replay_pending)
{
	struct btrfs_wib *wib = fs_info->wib;
	struct wib_recovery_stats st = { 0 };
	struct scrub_ctx *sctx;
	u64 last_start = 0;
	u64 last_len = 0;
	int ret;

	if (!wib)
		return 0;
	/*
	 * The live table becomes the record a stripe at a time, as each is
	 * taken over (see pending_taken); until then @pending answers for it.
	 */
	if (READ_ONCE(recover_drops_refusals) || !wib->nr_pending)
		wib_stop_consulting_pending(wib);
	if (!wib->nr_pending)
		return 0;
	if (!wib->pending_taken) {
		u64 *taken = kvcalloc(wib->nr_pending, sizeof(*taken), GFP_KERNEL);
		unsigned long flags;

		if (!taken) {
			wib_recovery_stopped(wib, -ENOMEM);
			return -ENOMEM;
		}
		spin_lock_irqsave(&wib->lock, flags);
		wib->pending_taken = taken;
		spin_unlock_irqrestore(&wib->lock, flags);
	}

	/* One scrub context and workqueue for the whole pass, not one each. */
	sctx = btrfs_scrub_raid56_recovery_begin(fs_info);
	if (IS_ERR(sctx)) {
		wib_recovery_stopped(wib, PTR_ERR(sctx));
		return PTR_ERR(sctx);
	}

	for (unsigned int i = 0; i < wib->nr_pending; i++) {
		const struct btrfs_wib_entry *e = &wib->pending[i];
		const u64 bits = e->bitmap | e->sticky;

		for (unsigned int bit = 0; bit < 64; bit++) {
			const u64 logical = e->bytenr + ((u64)bit << BTRFS_WIB_BLOCK_SHIFT);
			enum btrfs_raid56_recover_mode mode;
			unsigned int unwritten_par;
			bool error;
			u64 start;
			u64 len;

			if (!(bits & (1ULL << bit)))
				continue;
			/* Already handled as part of the previous full stripe. */
			if (last_len && logical >= last_start &&
			    logical < last_start + last_len)
				continue;

			/*
			 * A large log can take a long time to replay.  Stay
			 * killable: the on-disk log is only rewritten after
			 * the whole pass, so aborting here leaves a superset
			 * and the next mount redoes the work.
			 */
			if (fatal_signal_pending(current) ||
			    btrfs_fs_closing(fs_info)) {
				btrfs_warn(fs_info,
	"raid56 write-intent log: recovery interrupted, it will be redone at the next mount");
				ret = -EINTR;
				goto out;
			}

			ret = btrfs_raid56_full_stripe_range(fs_info, logical, &start, &len);
			if (ret == -ENOENT) {
				st.skipped++;
				continue;
			}
			if (ret < 0)
				goto out;
			last_start = start;
			last_len = len;

			/*
			 * An error record means a write to this stripe
			 * completed with a device error, so a sector of it may
			 * be stale while the parity holds what was
			 * acknowledged.  Only verified sectors may be trusted:
			 * recomputing the parity from a sector the scrub
			 * cannot check would overwrite the copy that still has
			 * the acknowledged content.  A sector without a
			 * checksum is exactly such a sector -- see
			 * scrub_verify_one_sector(), which has "no other
			 * choice but to trust it" -- so on a nodatacow file
			 * this destroys data that was still recoverable.
			 *
			 * This is only about error records.  An in-flight
			 * record is a crash in the middle of an RMW, where no
			 * device reported anything and the data on disk is
			 * what the filesystem should present; recomputing the
			 * parity from it is right.  The log keeps the two in
			 * separate fields (bitmap and sticky) precisely so
			 * they can be told apart.
			 *
			 * So an error record is recovered exactly as a user
			 * scrub would repair it: from the record, which says
			 * which side is stale, declining what it cannot
			 * decide.  The scrub reads the record from the live
			 * table, and at mount it is still only in the pending
			 * set, so put it there first.  With a tree log still to
			 * replay, extents only it knows are invisible to the
			 * scrub, so only verify now and finish after the replay.
			 */
			error = wib_pending_has_error(wib, start, len);
			if (!error)
				mode = BTRFS_RAID56_RECOVER_TRUSTED;
			else if (log_replay_pending)
				mode = BTRFS_RAID56_RECOVER_VERIFY;
			else
				mode = wib_error_mode();
			if (mode == BTRFS_RAID56_RECOVER_SCRUB) {
				btrfs_wib_add_sticky(fs_info, start, len);
				wib_readd_stale(fs_info, start, len);
			}
			/*
			 * Taken over, now that the live table holds what it
			 * needs of the record: the recovery's own reads of the
			 * stripe must not be refused as unrecovered, and its
			 * verdict decides what the stripe is from here on.
			 * Until that is in, anyone else's read of it is
			 * refused as before (btrfs_wib_recovering()) if a
			 * write into it may have been torn.
			 */
			if ((READ_ONCE(all_records_torn) ||
			     wib_pending_in_flight(wib, last_start, last_len)) &&
			    !READ_ONCE(recovering_stripe_readable))
				wib_set_recovering(wib, last_start, last_len);
			btrfs_wib_take_pending(wib, last_start, last_len, true);
			ret = wib_recover_one(fs_info, sctx, start, mode, log_replay_pending,
					      &start, &len, &unwritten_par, &st);
			if (ret < 0) {
				btrfs_wib_take_pending(wib, last_start, last_len, false);
				wib_set_recovering(wib, 0, 0);
				goto out;
			}
			if (mode == BTRFS_RAID56_RECOVER_SCRUB) {
				if (ret == 0)
					btrfs_wib_clear_sticky(fs_info, last_start, last_len);
			} else if (ret == 1) {
				btrfs_wib_add_sticky(fs_info, start, len);
				wib_readd_stale(fs_info, start, len);
			}
			/*
			 * Kept, and possibly torn as loaded: it stays so, or
			 * the next mount takes it for a plain failed write --
			 * unless all it was kept for is a parity on a missing
			 * device, which is then what is recorded, or a data
			 * column there it decided (BTRFS_WIB_STRIPE_DECIDED).
			 */
			if (ret == 1 && len && wib_pending_in_flight(wib, start, len)) {
				if (unwritten_par)
					btrfs_wib_parity_unwritten(fs_info, start, len,
								   unwritten_par);
				else
					wib_keep_torn(wib, start, len);
			}
			/* The verdict is in: the live table answers now. */
			wib_set_recovering(wib, 0, 0);
		}
	}

	btrfs_info(fs_info,
	"raid56 write-intent log: recovery done, %u full stripes scrubbed, %u skipped, %u kept recorded for a later pass, %u of those unrepairable now",
		   st.done, st.skipped, st.kept, st.failed);

	/* From here the live table is the record; see consult_pending. */
	wib_stop_consulting_pending(wib);
	kvfree(wib->pending);
	wib->pending = NULL;
	wib->nr_pending = 0;
	wib->max_pending = 0;
	kvfree(wib->pending_taken);
	wib->pending_taken = NULL;

	/*
	 * Make the regenerated parity durable and overwrite both on-disk
	 * slots with the new set, so that a later crash doesn't redo the
	 * work.  This is done whether or not the log stays enabled: a stale
	 * valid block would otherwise be replayed at every mount.
	 */
	ret = wib_persist_all_slots(wib, BTRFS_WIB_NR_SLOTS);
out:
	/* Stopped short of the end: @pending answers for what is left. */
	if (ret < 0 && wib->pending)
		wib_recovery_stopped(wib, ret);
	btrfs_scrub_raid56_recovery_end(fs_info, sctx);
	return ret;
}

/*
 * The tree log has been replayed: every extent is visible in the commit
 * roots now.  Complete the recovery of the stripes kept recorded by
 * btrfs_wib_recover() because their unverifiable sectors could not be
 * trusted; they can be verified now.
 */
int btrfs_wib_recover_after_replay(struct btrfs_fs_info *fs_info)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	struct wib_recovery_stats st = { 0 };
	struct btrfs_wib_entry *snap;
	struct scrub_ctx *sctx;
	unsigned int nr = 0;
	u64 last_start = 0;
	u64 last_len = 0;
	int ret = 0;

	if (!wib)
		return 0;

	snap = kvcalloc(BTRFS_WIB_NR_ENTRIES, sizeof(*snap), GFP_KERNEL);
	if (!snap)
		return -ENOMEM;
	spin_lock_irqsave(&wib->lock, flags);
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		if (wib->entries[i].sticky)
			snap[nr++] = wib->entries[i];
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	if (nr == 0)
		goto out;

	sctx = btrfs_scrub_raid56_recovery_begin(fs_info);
	if (IS_ERR(sctx)) {
		ret = PTR_ERR(sctx);
		goto out;
	}

	for (unsigned int i = 0; i < nr; i++) {
		const struct btrfs_wib_entry *e = &snap[i];

		for (unsigned int bit = 0; bit < 64; bit++) {
			const u64 logical = e->bytenr + ((u64)bit << BTRFS_WIB_BLOCK_SHIFT);
			unsigned int unwritten_par;
			u64 start;
			u64 len;

			if (!(e->sticky & (1ULL << bit)))
				continue;
			if (last_len && logical >= last_start &&
			    logical < last_start + last_len)
				continue;
			if (fatal_signal_pending(current) ||
			    btrfs_fs_closing(fs_info)) {
				btrfs_warn(fs_info,
	"raid56 write-intent log: recovery interrupted, it will be redone at the next mount");
				ret = -EINTR;
				goto out_end;
			}

			/*
			 * Never TRUSTED.  Every stripe this loop visits is
			 * here because it carries an ERROR record -- the loop
			 * above selects on e->sticky -- so a data sector of it
			 * may be stale while the parity holds what was
			 * acknowledged.  Regenerating every vertical stripe's
			 * parity from sectors the scrub cannot verify would,
			 * for nodatacow data, overwrite the only copy of the
			 * acknowledged value.  Decide as a scrub does instead:
			 * the record is in the live table already.
			 */
			ret = wib_recover_one(fs_info, sctx, logical,
					      wib_error_mode(), false,
					      &start, &len, &unwritten_par, &st);
			if (ret < 0)
				goto out_end;
			if (!len) {
				/* No RAID56 stripe there anymore. */
				btrfs_wib_clear_sticky(fs_info, logical, BTRFS_WIB_BLOCK_SIZE);
				continue;
			}
			last_start = start;
			last_len = len;
			if (ret == 0)
				btrfs_wib_clear_sticky(fs_info, start, len);
			/* As btrfs_wib_recover() does, for one it kept torn. */
			else if (ret == 1 && unwritten_par &&
				 btrfs_wib_stripe_torn(fs_info, start, len))
				btrfs_wib_parity_unwritten(fs_info, start, len,
							   unwritten_par);
		}
	}

	btrfs_info(fs_info,
	"raid56 write-intent log: recovery after log replay done, %u full stripes scrubbed, %u skipped, %u kept recorded for a later pass, %u of those unrepairable now",
		   st.done, st.skipped, st.kept, st.failed);
	ret = wib_persist_all_slots(wib, 1);
out_end:
	btrfs_scrub_raid56_recovery_end(fs_info, sctx);
out:
	kvfree(snap);
	return ret;
}

/*
 * A read-only mount that replays no tree log: nothing is written, so nothing
 * the log lists is recovered, and btrfs_wib_unrecovered() answers from it for
 * as long as the mount lasts.  Say so once, where the administrator salvaging
 * data off a degraded array -- mount -o ro,degraded is the usual first step --
 * will see why some of it reads as EIO.
 */
void btrfs_wib_ro_mount(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;

	if (!wib || !READ_ONCE(wib->pending_unrecovered))
		return;
	btrfs_warn(fs_info,
"raid56 write-intent log: this read-only mount does not recover the %u region(s) the log lists, where a write may have been torn; data without a checksum there that would have to be rebuilt with no parity left over to check the rebuild reads as EIO (read_unrecovered) until a read-write mount, degraded if need be, recovers them",
		   wib->nr_pending);
}

/*
 * Everything that has to happen when the filesystem becomes writable:
 * recover the logged stripes, then enable the log if the feature flag is
 * set or if the filesystem uses RAID56 and the user didn't opt out.
 *
 * Also used, with @log_replay_pending and @rdonly, before a tree log is
 * replayed on a read-only mount (the replay writes to the devices); the
 * feature flag is not set in that case.
 */
int btrfs_wib_rw_mount(struct btrfs_fs_info *fs_info, bool log_replay_pending,
		       bool rdonly)
{
	struct btrfs_wib *wib = fs_info->wib;
	int ret;

	if (!wib)
		return 0;
	/* Read-only media: nothing can be written, the log replay fails too. */
	if (rdonly && fs_info->fs_devices->rw_devices == 0) {
		btrfs_wib_ro_mount(fs_info);
		return 0;
	}

	if (btrfs_fs_compat_ro(fs_info, RAID56_WRITE_INTENT) && btrfs_is_zoned(fs_info)) {
		btrfs_err(fs_info,
			  "raid56 write-intent log is not supported on zoned filesystems");
		return -EOPNOTSUPP;
	}

	/* Recovery must precede any other write. */
	ret = btrfs_wib_recover(fs_info, log_replay_pending);
	if (ret)
		return ret;

	if (btrfs_fs_compat_ro(fs_info, RAID56_WRITE_INTENT) ||
	    (rdonly && btrfs_fs_incompat(fs_info, RAID56) && !btrfs_is_zoned(fs_info))) {
		ret = btrfs_wib_enable(fs_info);
		if (ret)
			return ret;
	}

	if (!rdonly &&
	    !btrfs_fs_compat_ro(fs_info, RAID56_WRITE_INTENT) &&
	    btrfs_fs_incompat(fs_info, RAID56) &&
	    !btrfs_test_opt(fs_info, NORAID56_WRITE_INTENT) &&
	    !btrfs_is_zoned(fs_info)) {
		btrfs_info(fs_info,
	"enabling raid56 write-intent log, older kernels will only mount this filesystem read-only");
		ret = btrfs_wib_enable(fs_info);
		if (ret)
			return ret;
		btrfs_set_fs_compat_ro(fs_info, RAID56_WRITE_INTENT);
	}
	return 0;
}
