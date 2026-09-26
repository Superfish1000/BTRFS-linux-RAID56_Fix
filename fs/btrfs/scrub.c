// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2011, 2012 STRATO.  All rights reserved.
 */

#include <linux/blkdev.h>
#include <linux/ratelimit.h>
#include <linux/sched/mm.h>
#include <linux/raid/pq.h>
#include <linux/raid/xor.h>
#include "ctree.h"
#include "discard.h"
#include "volumes.h"
#include "disk-io.h"
#include "ordered-data.h"
#include "transaction.h"
#include "backref.h"
#include "extent_io.h"
#include "dev-replace.h"
#include "raid56.h"
#include "block-group.h"
#include "zoned.h"
#include "fs.h"
#include "accessors.h"
#include "file-item.h"
#include "scrub.h"
#include "raid-stripe-tree.h"
#include "raid56-wib.h"

/*
 * This is only the first step towards a full-features scrub. It reads all
 * extent and super block and verifies the checksums. In case a bad checksum
 * is found or the extent cannot be read, good data will be written back if
 * any can be found.
 *
 * Future enhancements:
 *  - In case an unrepairable extent is encountered, track which files are
 *    affected and report them
 *  - track and record media errors, throw out bad devices
 *  - add a mode to also read unallocated space
 */

struct scrub_ctx;

/*
 * The following value only influences the performance.
 *
 * This determines how many stripes would be submitted in one go,
 * which is 512KiB (BTRFS_STRIPE_LEN * SCRUB_STRIPES_PER_GROUP).
 */
#define SCRUB_STRIPES_PER_GROUP		8

/*
 * How many groups we have for each sctx.
 *
 * This would be 8M per device, the same value as the old scrub in-flight bios
 * size limit.
 */
#define SCRUB_GROUPS_PER_SCTX		16

#define SCRUB_TOTAL_STRIPES		(SCRUB_GROUPS_PER_SCTX * SCRUB_STRIPES_PER_GROUP)

/* Represent one sector and its needed info to verify the content. */
struct scrub_sector_verification {
	union {
		/*
		 * Csum pointer for data csum verification.  Should point to a
		 * sector csum inside scrub_stripe::csums.
		 *
		 * NULL if this data sector has no csum.
		 */
		u8 *csum;

		/*
		 * Extra info for metadata verification.  All sectors inside a
		 * tree block share the same generation.
		 */
		u64 generation;
	};
};

enum scrub_stripe_flags {
	/* Set when @mirror_num, @dev, @physical and @logical are set. */
	SCRUB_STRIPE_FLAG_INITIALIZED,

	/* Set when the read-repair is finished. */
	SCRUB_STRIPE_FLAG_REPAIR_DONE,

	/*
	 * Set for data stripes if it's triggered from P/Q stripe.
	 * During such scrub, we should not report errors in data stripes, nor
	 * update the accounting.
	 */
	SCRUB_STRIPE_FLAG_NO_REPORT,

	/*
	 * RAID5/6 device replace: the target gets every sector of this data
	 * column, not only the ones an extent covers.  See
	 * scrub_replace_copy_column().
	 */
	SCRUB_STRIPE_FLAG_WHOLE_COLUMN,
};

/*
 * We have multiple bitmaps for one scrub_stripe.
 * However each bitmap has at most (BTRFS_STRIPE_LEN / blocksize) bits,
 * which is normally 16, and much smaller than BITS_PER_LONG (32 or 64).
 *
 * So to reduce memory usage for each scrub_stripe, we pack those bitmaps
 * into a larger one.
 *
 * These enum records where the sub-bitmap are inside the larger one.
 * Each subbitmap starts at scrub_bitmap_nr_##name * nr_sectors bit.
 */
enum {
	/* Which blocks are covered by extent items. */
	scrub_bitmap_nr_has_extent = 0,

	/* Which blocks are metadata. */
	scrub_bitmap_nr_is_metadata,

	/*
	 * Which blocks have errors, including IO, csum, and metadata
	 * errors.
	 * This sub-bitmap is the OR results of the next few error related
	 * sub-bitmaps.
	 */
	scrub_bitmap_nr_error,
	scrub_bitmap_nr_io_error,
	scrub_bitmap_nr_csum_error,
	scrub_bitmap_nr_meta_error,
	scrub_bitmap_nr_meta_gen_error,
	scrub_bitmap_nr_last,
};

/*
 * Represent one contiguous range with a length of BTRFS_STRIPE_LEN.
 */
struct scrub_stripe {
	struct scrub_ctx *sctx;
	struct btrfs_block_group *bg;
	struct scrub_sector_verification *sectors;
	struct btrfs_device *dev;

	void *buffer;

	u64 logical;
	u64 physical;

	u16 mirror_num;

	/* Should be BTRFS_STRIPE_LEN / sectorsize. */
	u16 nr_sectors;

	/*
	 * How many data/meta extents are in this stripe.  Only for scrub status
	 * reporting purposes.
	 */
	u16 nr_data_extents;
	u16 nr_meta_extents;

	atomic_t pending_io;
	wait_queue_head_t io_wait;
	wait_queue_head_t repair_wait;

	/*
	 * Indicate the states of the stripe.  Bits are defined in
	 * scrub_stripe_flags enum.
	 */
	unsigned long state;

	/* The large bitmap contains all the sub-bitmaps. */
	unsigned long bitmaps[BITS_TO_LONGS(scrub_bitmap_nr_last *
					    (BTRFS_STRIPE_LEN / BTRFS_MIN_BLOCKSIZE))];

	/*
	 * For writeback (repair or replace) error reporting.
	 * This one is protected by a spinlock, thus can not be packed into
	 * the larger bitmap.
	 */
	unsigned long write_error_bitmap;

	/* Writeback can be concurrent, thus we need to protect the bitmap. */
	spinlock_t write_error_lock;

	/*
	 * Checksum for the whole stripe if this stripe is inside a data block
	 * group.
	 */
	u8 *csums;

	/*
	 * The write-intent log knows this data column's last write did not
	 * reach the disk, and the sectors that carry no checksum of their own
	 * have nothing else that could say so.  Rebuild those from the parity
	 * rather than believing them: set after verification in
	 * scrub_stripe_read_repair_worker(), because scrub_verify_one_sector()
	 * clears the error bit of an unchecksummed sector before that
	 * ("we have no other choice but to trust it"), and set again after
	 * every re-read of this mirror for the same reason
	 * (scrub_verify_repair_read()).
	 *
	 * Only set where the rebuild is PROVABLE -- see
	 * scrub_raid56_parity_stripe(), which decides per full stripe.
	 */
	bool wib_rebuild;

	/*
	 * With SCRUB_STRIPE_FLAG_WHOLE_COLUMN: the full stripe this column
	 * belongs to and how many data columns it has, which is what the
	 * write-intent log is asked about and what it records against.
	 */
	u64 raid56_full_stripe;
	u16 raid56_nr_data;

	struct work_struct work;
};

struct scrub_ctx {
	struct scrub_stripe	stripes[SCRUB_TOTAL_STRIPES];
	struct scrub_stripe	*raid56_data_stripes;
	/* How many of them are allocated, 0 when the array is not there. */
	int			nr_raid56_data_stripes;
	struct btrfs_fs_info	*fs_info;
	struct btrfs_path	extent_path;
	struct btrfs_path	csum_path;
	int			first_free;
	int			cur_stripe;
	atomic_t		cancel_req;
	int			readonly;

	/* State of IO submission throttling affecting the associated device */
	ktime_t			throttle_deadline;
	u64			throttle_sent;

	bool			is_dev_replace;
	/*
	 * Write-intent log recovery at mount: not cancellable, and a failed
	 * parity write must fail the stripe (see btrfs_scrub_raid56_full_stripe()).
	 */
	bool			internal;
	/*
	 * Set by btrfs_scrub_raid56_full_stripe() in RECOVER_SCRUB mode.  The
	 * decision is per full stripe, but scrub_raid56_parity_stripe() runs
	 * once per parity device and would retire the record after the first
	 * pass -- before a RAID6's second parity has been written.  So it
	 * leaves retiring to the caller and says whether it would have.
	 */
	bool			raid56_defer_retire;
	bool			raid56_keep_record;
	/*
	 * Set by btrfs_scrub_raid56_full_stripe() for the one full stripe it
	 * recovers: a write into it may have been torn (its @torn), so its
	 * parity may not describe its data.  scrub_raid56_plan_wib() then
	 * declines a rebuild no parity is left over to check
	 * (SCRUB_WIB_TORN), and scrub_raid56_parity_stripe() says so in
	 * @raid56_torn_undecided.
	 */
	bool			raid56_torn;
	bool			raid56_torn_undecided;
	/*
	 * Set by scrub_raid56_recover_absent() for its verify pass, which only
	 * reads what its classification decides from: act on no record.
	 */
	bool			raid56_verify_only;
	/*
	 * Set by scrub_raid56_absent_pq() when it decided the data column on a
	 * missing device and regenerated both parities from the data: nothing
	 * in the stripe is left that a torn write could be in.
	 */
	bool			raid56_absent_decided;
	/*
	 * Device replace of a RAID5/6 chunk: copy whole data columns.  Decided
	 * once per chunk by scrub_enumerate_chunks(), which also has to make
	 * the chunk's content stable for it; see scrub_replace_copies_column().
	 */
	bool			raid56_whole_column;
	u64			write_pointer;

	struct mutex            wr_lock;
	struct btrfs_device     *wr_tgtdev;

	/*
	 * statistics
	 */
	struct btrfs_scrub_progress stat;
	spinlock_t		stat_lock;

	/*
	 * Use a ref counter to avoid use-after-free issues. Scrub workers
	 * decrement bios_in_flight and workers_pending and then do a wakeup
	 * on the list_wait wait queue. We must ensure the main scrub task
	 * doesn't free the scrub context before or while the workers are
	 * doing the wakeup() call.
	 */
	refcount_t              refs;
};

static_assert(BTRFS_STRIPE_LEN >= PAGE_SIZE);
static_assert(IS_ALIGNED(BTRFS_STRIPE_LEN, PAGE_SIZE));

#define scrub_calc_start_bit(stripe, name, block_nr)			\
({									\
	unsigned int __start_bit;					\
									\
	ASSERT(block_nr < stripe->nr_sectors,				\
		"nr_sectors=%u block_nr=%u", stripe->nr_sectors, block_nr); \
	__start_bit = scrub_bitmap_nr_##name * stripe->nr_sectors + block_nr; \
	__start_bit;							\
})

#define IMPLEMENT_SCRUB_BITMAP_OPS(name)				\
static inline void scrub_bitmap_set_##name(struct scrub_stripe *stripe,	\
				    unsigned int block_nr,		\
				    unsigned int nr_blocks)		\
{									\
	const unsigned int start_bit = scrub_calc_start_bit(stripe,	\
							    name, block_nr); \
									\
	bitmap_set(stripe->bitmaps, start_bit, nr_blocks);		\
}									\
static inline void scrub_bitmap_clear_##name(struct scrub_stripe *stripe, \
				      unsigned int block_nr,		\
				      unsigned int nr_blocks)		\
{									\
	const unsigned int start_bit = scrub_calc_start_bit(stripe, name, \
							    block_nr);	\
									\
	bitmap_clear(stripe->bitmaps, start_bit, nr_blocks);		\
}									\
static inline bool scrub_bitmap_test_bit_##name(struct scrub_stripe *stripe, \
				     unsigned int block_nr)		\
{									\
	const unsigned int start_bit = scrub_calc_start_bit(stripe, name, \
							    block_nr);	\
									\
	return test_bit(start_bit, stripe->bitmaps);			\
}									\
static inline void scrub_bitmap_set_bit_##name(struct scrub_stripe *stripe, \
				     unsigned int block_nr)		\
{									\
	const unsigned int start_bit = scrub_calc_start_bit(stripe, name, \
							    block_nr);	\
									\
	set_bit(start_bit, stripe->bitmaps);				\
}									\
static inline void scrub_bitmap_clear_bit_##name(struct scrub_stripe *stripe, \
				     unsigned int block_nr)		\
{									\
	const unsigned int start_bit = scrub_calc_start_bit(stripe, name, \
							    block_nr);	\
									\
	clear_bit(start_bit, stripe->bitmaps);				\
}									\
static inline unsigned long scrub_bitmap_read_##name(struct scrub_stripe *stripe) \
{									\
	const unsigned int nr_blocks = stripe->nr_sectors;		\
									\
	ASSERT(nr_blocks > 0 && nr_blocks <= BITS_PER_LONG,		\
	       "nr_blocks=%u BITS_PER_LONG=%u",				\
	       nr_blocks, BITS_PER_LONG);				\
									\
	return bitmap_read(stripe->bitmaps, nr_blocks * scrub_bitmap_nr_##name, \
			   stripe->nr_sectors);				\
}									\
static inline bool scrub_bitmap_empty_##name(struct scrub_stripe *stripe) \
{									\
	unsigned long bitmap = scrub_bitmap_read_##name(stripe);	\
									\
	return bitmap_empty(&bitmap, stripe->nr_sectors);		\
}									\
static inline unsigned int scrub_bitmap_weight_##name(struct scrub_stripe *stripe) \
{									\
	unsigned long bitmap = scrub_bitmap_read_##name(stripe);	\
									\
	return bitmap_weight(&bitmap, stripe->nr_sectors);		\
}
IMPLEMENT_SCRUB_BITMAP_OPS(has_extent);
IMPLEMENT_SCRUB_BITMAP_OPS(is_metadata);
IMPLEMENT_SCRUB_BITMAP_OPS(error);
IMPLEMENT_SCRUB_BITMAP_OPS(io_error);
IMPLEMENT_SCRUB_BITMAP_OPS(csum_error);
IMPLEMENT_SCRUB_BITMAP_OPS(meta_error);
IMPLEMENT_SCRUB_BITMAP_OPS(meta_gen_error);

struct scrub_warning {
	struct btrfs_path	*path;
	u64			extent_item_size;
	const char		*errstr;
	u64			physical;
	u64			logical;
	struct btrfs_device	*dev;
};

struct scrub_error_records {
	/*
	 * Bitmap recording which blocks hit errors (IO/csum/...) during the
	 * initial read.
	 */
	unsigned long init_error_bitmap;

	unsigned int nr_io_errors;
	unsigned int nr_csum_errors;
	unsigned int nr_meta_errors;
	unsigned int nr_meta_gen_errors;
};

static void release_scrub_stripe(struct scrub_stripe *stripe)
{
	if (!stripe)
		return;

	kvfree(stripe->buffer);
	kfree(stripe->sectors);
	kfree(stripe->csums);
	stripe->buffer = NULL;
	stripe->sectors = NULL;
	stripe->csums = NULL;
	stripe->sctx = NULL;
	stripe->state = 0;
}

static int init_scrub_stripe(struct btrfs_fs_info *fs_info,
			     struct scrub_stripe *stripe)
{
	memset(stripe, 0, sizeof(*stripe));

	stripe->nr_sectors = BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits;
	stripe->state = 0;

	init_waitqueue_head(&stripe->io_wait);
	init_waitqueue_head(&stripe->repair_wait);
	atomic_set(&stripe->pending_io, 0);
	spin_lock_init(&stripe->write_error_lock);

	stripe->buffer = kvmalloc(BTRFS_STRIPE_LEN, GFP_NOFS);
	if (!stripe->buffer)
		goto error;

	stripe->sectors = kzalloc_objs(struct scrub_sector_verification,
				       stripe->nr_sectors);
	if (!stripe->sectors)
		goto error;

	stripe->csums = kcalloc(BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits,
				fs_info->csum_size, GFP_KERNEL);
	if (!stripe->csums)
		goto error;
	return 0;
error:
	release_scrub_stripe(stripe);
	return -ENOMEM;
}

static void wait_scrub_stripe_io(struct scrub_stripe *stripe)
{
	wait_event(stripe->io_wait, atomic_read(&stripe->pending_io) == 0);
}

static void scrub_put_ctx(struct scrub_ctx *sctx);

static void __scrub_blocked_if_needed(struct btrfs_fs_info *fs_info)
{
	while (atomic_read(&fs_info->scrub_pause_req)) {
		mutex_unlock(&fs_info->scrub_lock);
		wait_event(fs_info->scrub_pause_wait,
		   atomic_read(&fs_info->scrub_pause_req) == 0);
		mutex_lock(&fs_info->scrub_lock);
	}
}

static void scrub_pause_on(struct btrfs_fs_info *fs_info)
{
	atomic_inc(&fs_info->scrubs_paused);
	wake_up(&fs_info->scrub_pause_wait);
}

static void scrub_pause_off(struct btrfs_fs_info *fs_info)
{
	mutex_lock(&fs_info->scrub_lock);
	__scrub_blocked_if_needed(fs_info);
	atomic_dec(&fs_info->scrubs_paused);
	mutex_unlock(&fs_info->scrub_lock);

	wake_up(&fs_info->scrub_pause_wait);
}

static void scrub_blocked_if_needed(struct btrfs_fs_info *fs_info)
{
	scrub_pause_on(fs_info);
	scrub_pause_off(fs_info);
}

static void scrub_free_raid56_data_stripes(struct scrub_ctx *sctx)
{
	if (!sctx->raid56_data_stripes)
		return;
	for (int i = 0; i < sctx->nr_raid56_data_stripes; i++)
		release_scrub_stripe(&sctx->raid56_data_stripes[i]);
	kfree(sctx->raid56_data_stripes);
	sctx->raid56_data_stripes = NULL;
	sctx->nr_raid56_data_stripes = 0;
}

/*
 * Make sure @sctx carries @nr_data_stripes data stripes for the chunk about
 * to be scrubbed, reusing the ones it already has.  Chunks of one filesystem
 * can differ in width, so the array is only reallocated when the count does
 * not match.
 */
static int scrub_alloc_raid56_data_stripes(struct scrub_ctx *sctx,
					   struct btrfs_block_group *bg,
					   int nr_data_stripes)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;

	if (sctx->nr_raid56_data_stripes != nr_data_stripes) {
		scrub_free_raid56_data_stripes(sctx);
		sctx->raid56_data_stripes = kzalloc_objs(struct scrub_stripe,
							 nr_data_stripes);
		if (!sctx->raid56_data_stripes)
			return -ENOMEM;
		sctx->nr_raid56_data_stripes = nr_data_stripes;
		for (int i = 0; i < nr_data_stripes; i++) {
			int ret;

			ret = init_scrub_stripe(fs_info,
						&sctx->raid56_data_stripes[i]);
			if (ret < 0) {
				/*
				 * Do not leave a short array behind that a
				 * retry with the same width would take for
				 * a usable one.
				 */
				scrub_free_raid56_data_stripes(sctx);
				return ret;
			}
			sctx->raid56_data_stripes[i].sctx = sctx;
		}
	}
	/*
	 * A reused stripe still points at the previous chunk's block group,
	 * which scrub_reset_stripe() does not clear and stripe_length() reads
	 * for the geometry.  scrub_raid56_parity_stripe() resets everything
	 * else it uses.
	 */
	for (int i = 0; i < nr_data_stripes; i++)
		sctx->raid56_data_stripes[i].bg = bg;
	return 0;
}

static noinline_for_stack void scrub_free_ctx(struct scrub_ctx *sctx)
{
	int i;

	if (!sctx)
		return;

	scrub_free_raid56_data_stripes(sctx);
	for (i = 0; i < SCRUB_TOTAL_STRIPES; i++)
		release_scrub_stripe(&sctx->stripes[i]);

	kvfree(sctx);
}

static void scrub_put_ctx(struct scrub_ctx *sctx)
{
	if (refcount_dec_and_test(&sctx->refs))
		scrub_free_ctx(sctx);
}

static noinline_for_stack struct scrub_ctx *scrub_setup_ctx(
		struct btrfs_fs_info *fs_info, bool is_dev_replace)
{
	struct scrub_ctx *sctx;
	int		i;

	/* Since sctx has inline 128 stripes, it can go beyond 64K easily.  Use
	 * kvzalloc().
	 */
	sctx = kvzalloc_obj(*sctx);
	if (!sctx)
		goto nomem;
	refcount_set(&sctx->refs, 1);
	sctx->is_dev_replace = is_dev_replace;
	sctx->fs_info = fs_info;
	sctx->extent_path.search_commit_root = true;
	sctx->extent_path.skip_locking = true;
	sctx->csum_path.search_commit_root = true;
	sctx->csum_path.skip_locking = true;
	for (i = 0; i < SCRUB_TOTAL_STRIPES; i++) {
		int ret;

		ret = init_scrub_stripe(fs_info, &sctx->stripes[i]);
		if (ret < 0)
			goto nomem;
		sctx->stripes[i].sctx = sctx;
	}
	sctx->first_free = 0;
	atomic_set(&sctx->cancel_req, 0);

	spin_lock_init(&sctx->stat_lock);
	sctx->throttle_deadline = 0;

	mutex_init(&sctx->wr_lock);
	if (is_dev_replace) {
		WARN_ON(!fs_info->dev_replace.tgtdev);
		sctx->wr_tgtdev = fs_info->dev_replace.tgtdev;
	}

	return sctx;

nomem:
	scrub_free_ctx(sctx);
	return ERR_PTR(-ENOMEM);
}

static int scrub_print_warning_inode(u64 inum, u64 offset, u64 num_bytes,
				     u64 root, void *warn_ctx)
{
	u32 nlink;
	int ret;
	int i;
	unsigned nofs_flag;
	struct extent_buffer *eb;
	struct btrfs_inode_item *inode_item;
	struct scrub_warning *swarn = warn_ctx;
	struct btrfs_fs_info *fs_info = swarn->dev->fs_info;
	struct inode_fs_paths *ipath __free(inode_fs_paths) = NULL;
	struct btrfs_root *local_root;
	struct btrfs_key key;

	local_root = btrfs_get_fs_root(fs_info, root, true);
	if (IS_ERR(local_root)) {
		ret = PTR_ERR(local_root);
		goto err;
	}

	/*
	 * this makes the path point to (inum INODE_ITEM ioff)
	 */
	key.objectid = inum;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, local_root, &key, swarn->path, 0, 0);
	if (ret) {
		btrfs_put_root(local_root);
		btrfs_release_path(swarn->path);
		goto err;
	}

	eb = swarn->path->nodes[0];
	inode_item = btrfs_item_ptr(eb, swarn->path->slots[0],
					struct btrfs_inode_item);
	nlink = btrfs_inode_nlink(eb, inode_item);
	btrfs_release_path(swarn->path);

	/*
	 * init_path might indirectly call vmalloc, or use GFP_KERNEL. Scrub
	 * uses GFP_NOFS in this context, so we keep it consistent but it does
	 * not seem to be strictly necessary.
	 */
	nofs_flag = memalloc_nofs_save();
	ipath = init_ipath(4096, local_root, swarn->path);
	memalloc_nofs_restore(nofs_flag);
	if (IS_ERR(ipath)) {
		btrfs_put_root(local_root);
		ret = PTR_ERR(ipath);
		ipath = NULL;
		goto err;
	}
	ret = paths_from_inode(inum, ipath);

	if (ret < 0)
		goto err;

	/*
	 * we deliberately ignore the bit ipath might have been too small to
	 * hold all of the paths here
	 */
	for (i = 0; i < ipath->fspath->elem_cnt; ++i)
		btrfs_warn(fs_info,
"scrub: %s at logical %llu on dev %s, physical %llu root %llu inode %llu offset %llu length %u links %u (path: %s)",
				  swarn->errstr, swarn->logical,
				  btrfs_dev_name(swarn->dev),
				  swarn->physical,
				  root, inum, offset,
				  fs_info->sectorsize, nlink,
				  (char *)(unsigned long)ipath->fspath->val[i]);

	btrfs_put_root(local_root);
	return 0;

err:
	btrfs_warn(fs_info,
			  "scrub: %s at logical %llu on dev %s, physical %llu root %llu inode %llu offset %llu: path resolving failed with ret=%d",
			  swarn->errstr, swarn->logical,
			  btrfs_dev_name(swarn->dev),
			  swarn->physical,
			  root, inum, offset, ret);

	return 0;
}

static void scrub_print_common_warning(const char *errstr, struct btrfs_device *dev,
				       bool is_super, u64 logical, u64 physical)
{
	struct btrfs_fs_info *fs_info = dev->fs_info;
	BTRFS_PATH_AUTO_FREE(path);
	struct btrfs_key found_key;
	struct extent_buffer *eb;
	struct btrfs_extent_item *ei;
	struct scrub_warning swarn;
	u64 flags = 0;
	u32 item_size;
	int ret;

	/* Super block error, no need to search extent tree. */
	if (is_super) {
		btrfs_warn(fs_info, "scrub: %s on device %s, physical %llu",
				  errstr, btrfs_dev_name(dev), physical);
		return;
	}
	path = btrfs_alloc_path();
	if (!path)
		return;

	swarn.physical = physical;
	swarn.logical = logical;
	swarn.errstr = errstr;
	swarn.dev = NULL;

	ret = extent_from_logical(fs_info, swarn.logical, path, &found_key,
				  &flags);
	if (ret < 0)
		return;

	swarn.extent_item_size = found_key.offset;

	eb = path->nodes[0];
	ei = btrfs_item_ptr(eb, path->slots[0], struct btrfs_extent_item);
	item_size = btrfs_item_size(eb, path->slots[0]);

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		unsigned long ptr = 0;
		u8 ref_level;
		u64 ref_root;

		while (true) {
			ret = tree_backref_for_extent(&ptr, eb, &found_key, ei,
						      item_size, &ref_root,
						      &ref_level);
			if (ret < 0) {
				btrfs_warn(fs_info,
		   "scrub: failed to resolve tree backref for logical %llu: %d",
					   swarn.logical, ret);
				break;
			}
			if (ret > 0)
				break;
			btrfs_warn(fs_info,
"scrub: %s at logical %llu on dev %s, physical %llu: metadata %s (level %d) in tree %llu",
				errstr, swarn.logical, btrfs_dev_name(dev),
				swarn.physical, (ref_level ? "node" : "leaf"),
				ref_level, ref_root);
		}
		btrfs_release_path(path);
	} else {
		struct btrfs_backref_walk_ctx ctx = { 0 };

		btrfs_release_path(path);

		ctx.bytenr = found_key.objectid;
		ctx.extent_item_pos = swarn.logical - found_key.objectid;
		ctx.fs_info = fs_info;

		swarn.path = path;
		swarn.dev = dev;

		iterate_extent_inodes(&ctx, true, scrub_print_warning_inode, &swarn);
	}
}

static int fill_writer_pointer_gap(struct scrub_ctx *sctx, u64 physical)
{
	int ret = 0;
	u64 length;

	if (!btrfs_is_zoned(sctx->fs_info))
		return 0;

	if (!btrfs_dev_is_sequential(sctx->wr_tgtdev, physical))
		return 0;

	if (sctx->write_pointer < physical) {
		length = physical - sctx->write_pointer;

		ret = btrfs_zoned_issue_zeroout(sctx->wr_tgtdev,
						sctx->write_pointer, length);
		if (!ret)
			sctx->write_pointer = physical;
	}
	return ret;
}

/*
 * Unlike the existing csum which is based on paddr, this version is fully on
 * vaddr, so no extra per-page iteration needed.
 */
static void scrub_calc_vaddr_csum(struct btrfs_fs_info *fs_info,
				  void *vaddr, unsigned int len, u8 *dest)
{
	struct btrfs_csum_ctx csum;

	btrfs_csum_init(&csum, fs_info->csum_type);
	btrfs_csum_update(&csum, vaddr, len);
	btrfs_csum_final(&csum, dest);
}

static void scrub_verify_one_metadata(struct scrub_stripe *stripe, int sector_nr)
{
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	const u32 sectors_per_tree = fs_info->nodesize >> fs_info->sectorsize_bits;
	const u64 logical = stripe->logical + (sector_nr << fs_info->sectorsize_bits);
	void *first_vaddr = stripe->buffer + (sector_nr << fs_info->sectorsize_bits);
	struct btrfs_header *header = first_vaddr;
	u8 calculated_csum[BTRFS_CSUM_SIZE];

	if (logical != btrfs_stack_header_bytenr(header)) {
		scrub_bitmap_set_meta_error(stripe, sector_nr, sectors_per_tree);
		scrub_bitmap_set_error(stripe, sector_nr, sectors_per_tree);
		btrfs_warn_rl(fs_info,
	  "scrub: tree block %llu mirror %u has bad bytenr, has %llu want %llu",
			      logical, stripe->mirror_num,
			      btrfs_stack_header_bytenr(header), logical);
		return;
	}
	if (memcmp(header->fsid, fs_info->fs_devices->metadata_uuid,
		   BTRFS_FSID_SIZE) != 0) {
		scrub_bitmap_set_meta_error(stripe, sector_nr, sectors_per_tree);
		scrub_bitmap_set_error(stripe, sector_nr, sectors_per_tree);
		btrfs_warn_rl(fs_info,
	      "scrub: tree block %llu mirror %u has bad fsid, has %pU want %pU",
			      logical, stripe->mirror_num,
			      header->fsid, fs_info->fs_devices->metadata_uuid);
		return;
	}
	if (memcmp(header->chunk_tree_uuid, fs_info->chunk_tree_uuid,
		   BTRFS_UUID_SIZE) != 0) {
		scrub_bitmap_set_meta_error(stripe, sector_nr, sectors_per_tree);
		scrub_bitmap_set_error(stripe, sector_nr, sectors_per_tree);
		btrfs_warn_rl(fs_info,
   "scrub: tree block %llu mirror %u has bad chunk tree uuid, has %pU want %pU",
			      logical, stripe->mirror_num,
			      header->chunk_tree_uuid, fs_info->chunk_tree_uuid);
		return;
	}

	/* Now check tree block csum. */
	scrub_calc_vaddr_csum(fs_info, first_vaddr + BTRFS_CSUM_SIZE,
			      fs_info->nodesize - BTRFS_CSUM_SIZE, calculated_csum);
	if (memcmp(calculated_csum, header->csum, fs_info->csum_size) != 0) {
		scrub_bitmap_set_meta_error(stripe, sector_nr, sectors_per_tree);
		scrub_bitmap_set_error(stripe, sector_nr, sectors_per_tree);
		btrfs_warn_rl(fs_info,
"scrub: tree block %llu mirror %u has bad csum, has " BTRFS_CSUM_FMT " want " BTRFS_CSUM_FMT,
			      logical, stripe->mirror_num,
			      BTRFS_CSUM_FMT_VALUE(fs_info->csum_size, header->csum),
			      BTRFS_CSUM_FMT_VALUE(fs_info->csum_size, calculated_csum));
		return;
	}
	if (stripe->sectors[sector_nr].generation !=
	    btrfs_stack_header_generation(header)) {
		scrub_bitmap_set_meta_gen_error(stripe, sector_nr, sectors_per_tree);
		scrub_bitmap_set_error(stripe, sector_nr, sectors_per_tree);
		btrfs_warn_rl(fs_info,
      "scrub: tree block %llu mirror %u has bad generation, has %llu want %llu",
			      logical, stripe->mirror_num,
			      btrfs_stack_header_generation(header),
			      stripe->sectors[sector_nr].generation);
		return;
	}
	scrub_bitmap_clear_error(stripe, sector_nr, sectors_per_tree);
	scrub_bitmap_clear_csum_error(stripe, sector_nr, sectors_per_tree);
	scrub_bitmap_clear_meta_error(stripe, sector_nr, sectors_per_tree);
	scrub_bitmap_clear_meta_gen_error(stripe, sector_nr, sectors_per_tree);
}

static void scrub_verify_one_sector(struct scrub_stripe *stripe, int sector_nr)
{
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	struct scrub_sector_verification *sector = &stripe->sectors[sector_nr];
	const u32 sectors_per_tree = fs_info->nodesize >> fs_info->sectorsize_bits;
	u8 csum_buf[BTRFS_CSUM_SIZE];

	ASSERT(sector_nr >= 0 && sector_nr < stripe->nr_sectors);

	/* Sector not utilized, skip it. */
	if (!scrub_bitmap_test_bit_has_extent(stripe, sector_nr))
		return;

	/* IO error, no need to check. */
	if (scrub_bitmap_test_bit_io_error(stripe, sector_nr))
		return;

	/* Metadata, verify the full tree block. */
	if (scrub_bitmap_test_bit_is_metadata(stripe, sector_nr)) {
		/*
		 * Check if the tree block crosses the stripe boundary.  If
		 * crossed the boundary, we cannot verify it but only give a
		 * warning.
		 *
		 * This can only happen on a very old filesystem where chunks
		 * are not ensured to be stripe aligned.
		 */
		if (unlikely(sector_nr + sectors_per_tree > stripe->nr_sectors)) {
			btrfs_warn_rl(fs_info,
			"scrub: tree block at %llu crosses stripe boundary %llu",
				      stripe->logical +
				      (sector_nr << fs_info->sectorsize_bits),
				      stripe->logical);
			return;
		}
		scrub_verify_one_metadata(stripe, sector_nr);
		return;
	}

	/*
	 * Data is easier, we just verify the data csum (if we have it).  For
	 * cases without csum, we have no other choice but to trust it.
	 */
	if (!sector->csum) {
		scrub_bitmap_clear_bit_error(stripe, sector_nr);
		return;
	}

	scrub_calc_vaddr_csum(fs_info,
			      stripe->buffer + (sector_nr << fs_info->sectorsize_bits),
			      fs_info->sectorsize, csum_buf);
	if (memcmp(csum_buf, sector->csum, fs_info->csum_size)) {
		scrub_bitmap_set_bit_csum_error(stripe, sector_nr);
		scrub_bitmap_set_bit_error(stripe, sector_nr);
	} else {
		scrub_bitmap_clear_bit_csum_error(stripe, sector_nr);
		scrub_bitmap_clear_bit_error(stripe, sector_nr);
	}
}

/* Verify specified sectors of a stripe. */
static void scrub_verify_one_stripe(struct scrub_stripe *stripe, unsigned long bitmap)
{
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	const u32 sectors_per_tree = fs_info->nodesize >> fs_info->sectorsize_bits;
	int sector_nr;

	for_each_set_bit(sector_nr, &bitmap, stripe->nr_sectors) {
		scrub_verify_one_sector(stripe, sector_nr);
		if (scrub_bitmap_test_bit_is_metadata(stripe, sector_nr))
			sector_nr += sectors_per_tree - 1;
	}
}

static unsigned int calc_sector_number(const struct btrfs_bio *bbio)
{
	const struct scrub_stripe *stripe = bbio->private;
	const struct btrfs_fs_info *fs_info = stripe->bg->fs_info;

	/* Scrub bbios all have their @file_offset set to the logical bytenr. */
	ASSERT(bbio->file_offset >= stripe->logical &&
	       bbio->file_offset < stripe->logical + (stripe->nr_sectors <<
						      fs_info->sectorsize_bits),
	       "scrub bio logical=%llu stripe logical=%llu stripe len=%u",
	       bbio->file_offset, stripe->logical,
	       stripe->nr_sectors << fs_info->sectorsize_bits);
	return (bbio->file_offset - stripe->logical) >> fs_info->sectorsize_bits;
}

/*
 * Common handling of read endio.
 *
 * The bbio will be released, so no more access to @bbio after this function.
 */
static void scrub_read_endio_common(struct btrfs_bio *bbio)
{
	struct scrub_stripe *stripe = bbio->private;
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	unsigned int sector_nr = calc_sector_number(bbio);
	const u32 bio_size = bio_get_size(&bbio->bio);
	const u32 sectors = bio_size >> fs_info->sectorsize_bits;


	/*
	 * For vmallocated space, readers need to call invalidate_kernel_vmap_range()
	 * to manage the coherency between kernel mapping and devie space mapping.
	 */
	if (is_vmalloc_addr(stripe->buffer))
		invalidate_kernel_vmap_range(
			stripe->buffer + (sector_nr << fs_info->sectorsize_bits),
			bio_size);

	if (bbio->bio.bi_status) {
		scrub_bitmap_set_io_error(stripe, sector_nr, sectors);
		scrub_bitmap_set_error(stripe, sector_nr, sectors);
	} else {
		scrub_bitmap_clear_io_error(stripe, sector_nr, sectors);
	}
	bio_put(&bbio->bio);
}

/*
 * Repair read is different to the regular read:
 *
 * - Only reads the failed sectors
 * - May have extra blocksize limits
 */
static void scrub_repair_read_endio(struct btrfs_bio *bbio)
{
	struct scrub_stripe *stripe = bbio->private;

	scrub_read_endio_common(bbio);

	if (atomic_dec_and_test(&stripe->pending_io))
		wake_up(&stripe->io_wait);
}

static int calc_next_mirror(int mirror, int num_copies)
{
	ASSERT(mirror <= num_copies);
	return (mirror + 1 > num_copies) ? 1 : mirror + 1;
}

static void scrub_bio_add_sector(struct btrfs_bio *bbio, struct scrub_stripe *stripe,
				 int sector_nr)
{
	struct btrfs_fs_info *fs_info = bbio->inode->root->fs_info;
	const u32 offset = sector_nr << fs_info->sectorsize_bits;
	int ret;

	ASSERT(offset + fs_info->sectorsize <= BTRFS_STRIPE_LEN);

	if (is_vmalloc_addr(stripe->buffer)) {
		ret = bio_add_vmalloc(&bbio->bio, stripe->buffer + offset, fs_info->sectorsize);
		ASSERT(ret == true);
		return;
	}
	ret = bio_add_page(&bbio->bio, virt_to_page(stripe->buffer + offset),
			   fs_info->sectorsize, offset_in_page(stripe->buffer + offset));
	ASSERT(ret == fs_info->sectorsize);
}

static struct btrfs_bio *alloc_scrub_bbio(struct btrfs_fs_info *fs_info,
					  blk_opf_t opf,
					  u64 logical,
					  btrfs_bio_end_io_t end_io, void *private)
{
	struct btrfs_bio *bbio;

	/*
	 * Stripe->buffer is allocated by kvmalloc(), which can be pages at
	 * different physical addresses, we have to ensure the bbio is large
	 * enough to contain the full stripe.
	 */
	bbio = btrfs_bio_alloc(BTRFS_STRIPE_LEN >> PAGE_SHIFT, opf,
			       BTRFS_I(fs_info->btree_inode),
			       logical, end_io, private);
	bbio->is_scrub = true;
	bbio->bio.bi_iter.bi_sector = logical >> SECTOR_SHIFT;
	return bbio;
}

/*
 * Testing only: raid56_wf_replace_refuses_free=1 lets a RAID5/6 device replace
 * rebuild the sectors of a column that no extent holds in the same reads as its
 * data, as before scrub_replace_splits_free(): in a full stripe the
 * write-intent log marks possibly torn the rebuild of the whole read is
 * refused, and the free sectors are counted as lost, raising replace_uncopyable
 * and leaving zeros and a stale mark on the new device.  The negative control
 * for uml/replace_torn_free.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool replace_refuses_free;
module_param_named(raid56_wf_replace_refuses_free, replace_refuses_free, bool, 0644);
MODULE_PARM_DESC(raid56_wf_replace_refuses_free,
		 "Let a RAID5/6 device replace rebuild the free sectors of a column in the same reads as its data, so that a stripe marked possibly torn refuses them and the replace reports them lost (testing only: restores a known defect)");
#else
static const bool replace_refuses_free;
#endif

/*
 * Does a device replace that rebuilds @stripe's RAID5/6 data column at @mirror
 * read the sectors no extent holds on their own, and say so
 * (@scrub_reads_free in struct btrfs_bio)?
 *
 * A rebuild of data without a checksum is refused where the full stripe may be
 * torn and no parity is left over to check it (recover_rbio()), and every
 * sector of a read goes with it.  A free sector counts as unchecked data there,
 * as the RAID5/6 layer cannot tell it from data -- so the replace of a missing
 * device after a crash counted every free sector of such a stripe as lost,
 * raised replace_uncopyable for data that was never there, and put zeros and a
 * stale mark on its target where the rebuild was exactly what belongs there.
 * Read apart, only the data is refused.
 */
static bool scrub_replace_splits_free(const struct scrub_stripe *stripe, int mirror)
{
	return stripe->sctx->is_dev_replace && mirror > 1 &&
	       (stripe->bg->flags & BTRFS_BLOCK_GROUP_RAID56_MASK) &&
	       (stripe->bg->flags & BTRFS_BLOCK_GROUP_DATA) &&
	       !READ_ONCE(replace_refuses_free);
}

static void scrub_stripe_submit_repair_read(struct scrub_stripe *stripe,
					    int mirror, int blocksize, bool wait)
{
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	struct btrfs_bio *bbio = NULL;
	const unsigned long old_error_bitmap = scrub_bitmap_read_error(stripe);
	const unsigned long has_extent = scrub_bitmap_read_has_extent(stripe);
	const bool split = scrub_replace_splits_free(stripe, mirror);
	int i;

	ASSERT(stripe->mirror_num >= 1, "stripe->mirror_num=%d", stripe->mirror_num);
	ASSERT(atomic_read(&stripe->pending_io) == 0,
	       "atomic_read(&stripe->pending_io)=%d", atomic_read(&stripe->pending_io));

	for_each_set_bit(i, &old_error_bitmap, stripe->nr_sectors) {
		const bool reads_free = split && !test_bit(i, &has_extent);

		/* The current sector cannot be merged, submit the bio. */
		if (bbio && ((i > 0 && !test_bit(i - 1, &old_error_bitmap)) ||
			     bbio->bio.bi_iter.bi_size >= blocksize ||
			     bbio->scrub_reads_free != reads_free)) {
			ASSERT(bbio->bio.bi_iter.bi_size);
			atomic_inc(&stripe->pending_io);
			btrfs_submit_bbio(bbio, mirror);
			if (wait)
				wait_scrub_stripe_io(stripe);
			bbio = NULL;
		}

		if (!bbio) {
			bbio = alloc_scrub_bbio(fs_info, REQ_OP_READ,
						stripe->logical + (i << fs_info->sectorsize_bits),
						scrub_repair_read_endio, stripe);
			bbio->scrub_reads_free = reads_free;
		}

		scrub_bio_add_sector(bbio, stripe, i);
	}
	if (bbio) {
		ASSERT(bbio->bio.bi_iter.bi_size);
		atomic_inc(&stripe->pending_io);
		btrfs_submit_bbio(bbio, mirror);
		if (wait)
			wait_scrub_stripe_io(stripe);
	}
}

static void scrub_stripe_report_errors(struct scrub_ctx *sctx,
				       struct scrub_stripe *stripe,
				       const struct scrub_error_records *errors)
{
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct btrfs_device *dev = NULL;
	const unsigned long extent_bitmap = scrub_bitmap_read_has_extent(stripe);
	const unsigned long error_bitmap = scrub_bitmap_read_error(stripe);
	/*
	 * A sector repaired in memory whose write-back failed is still wrong
	 * on the disk.  The repair worker waited for those writes, so this is
	 * complete.
	 */
	const unsigned long write_error_bitmap = stripe->write_error_bitmap &
						 errors->init_error_bitmap;
	u64 physical = 0;
	int nr_data_sectors = 0;
	int nr_meta_sectors = 0;
	int nr_nodatacsum_sectors = 0;
	int nr_repaired_sectors = 0;
	int nr_write_failed = 0;
	int sector_nr;

	if (test_bit(SCRUB_STRIPE_FLAG_NO_REPORT, &stripe->state))
		return;

	/*
	 * Init needed infos for error reporting.
	 *
	 * Although our scrub_stripe infrastructure is mostly based on btrfs_submit_bio()
	 * thus no need for dev/physical, error reporting still needs dev and physical.
	 */
	if (!bitmap_empty(&errors->init_error_bitmap, stripe->nr_sectors)) {
		u64 mapped_len = fs_info->sectorsize;
		struct btrfs_io_context *bioc = NULL;
		int stripe_index = stripe->mirror_num - 1;
		int ret;

		/* For scrub, our mirror_num should always start at 1. */
		ASSERT(stripe->mirror_num >= 1, "stripe->mirror_num=%d", stripe->mirror_num);
		ret = btrfs_map_block(fs_info, BTRFS_MAP_GET_READ_MIRRORS,
				      stripe->logical, &mapped_len, &bioc,
				      NULL, NULL);
		/*
		 * If we failed, dev will be NULL, and later detailed reports
		 * will just be skipped.
		 */
		if (ret < 0)
			goto skip;
		physical = bioc->stripes[stripe_index].physical;
		dev = bioc->stripes[stripe_index].dev;
		btrfs_put_bioc(bioc);
	}

skip:
	for_each_set_bit(sector_nr, &extent_bitmap, stripe->nr_sectors) {
		const u64 sector_logical = stripe->logical +
					   ((u64)sector_nr << fs_info->sectorsize_bits);
		const u64 sector_physical = physical +
					   ((u64)sector_nr << fs_info->sectorsize_bits);
		bool repaired = false;

		if (scrub_bitmap_test_bit_is_metadata(stripe, sector_nr)) {
			nr_meta_sectors++;
		} else {
			nr_data_sectors++;
			if (!stripe->sectors[sector_nr].csum)
				nr_nodatacsum_sectors++;
		}

		if (test_bit(sector_nr, &errors->init_error_bitmap) &&
		    !test_bit(sector_nr, &error_bitmap) &&
		    !test_bit(sector_nr, &write_error_bitmap)) {
			nr_repaired_sectors++;
			repaired = true;
		}

		/* Good sector from the beginning, nothing need to be done. */
		if (!test_bit(sector_nr, &errors->init_error_bitmap))
			continue;

		/*
		 * The right content was found, but the device did not take it:
		 * reported as corrected, this used to tell the user the disk
		 * was fixed while it still held the error.
		 */
		if (test_bit(sector_nr, &write_error_bitmap) &&
		    !test_bit(sector_nr, &error_bitmap)) {
			nr_write_failed++;
			if (dev)
				btrfs_err_rl(fs_info,
"scrub: unable to fixup error at logical %llu on dev %s physical %llu: the repair write failed",
					     sector_logical, btrfs_dev_name(dev),
					     sector_physical);
			else
				btrfs_err_rl(fs_info,
"scrub: unable to fixup error at logical %llu on mirror %u: the repair write failed",
					     sector_logical, stripe->mirror_num);
			continue;
		}

		/*
		 * Report error for the corrupted sectors.  If repaired, just
		 * output the message of repaired message.
		 */
		if (repaired) {
			if (dev) {
				btrfs_err_rl(fs_info,
		"scrub: fixed up error at logical %llu on dev %s physical %llu",
					    sector_logical, btrfs_dev_name(dev),
					    sector_physical);
			} else {
				btrfs_err_rl(fs_info,
			   "scrub: fixed up error at logical %llu on mirror %u",
					    sector_logical, stripe->mirror_num);
			}
			continue;
		}

		/* The remaining are all for unrepaired. */
		if (dev) {
			btrfs_err_rl(fs_info,
"scrub: unable to fixup (regular) error at logical %llu on dev %s physical %llu",
					    sector_logical, btrfs_dev_name(dev),
					    sector_physical);
		} else {
			btrfs_err_rl(fs_info,
	  "scrub: unable to fixup (regular) error at logical %llu on mirror %u",
					    sector_logical, stripe->mirror_num);
		}

		if (scrub_bitmap_test_bit_io_error(stripe, sector_nr))
			if (__ratelimit(&rs) && dev)
				scrub_print_common_warning("i/o error", dev, false,
						     sector_logical, sector_physical);
		if (scrub_bitmap_test_bit_csum_error(stripe, sector_nr))
			if (__ratelimit(&rs) && dev)
				scrub_print_common_warning("checksum error", dev, false,
						     sector_logical, sector_physical);
		if (scrub_bitmap_test_bit_meta_error(stripe, sector_nr))
			if (__ratelimit(&rs) && dev)
				scrub_print_common_warning("header error", dev, false,
						     sector_logical, sector_physical);
		if (scrub_bitmap_test_bit_meta_gen_error(stripe, sector_nr))
			if (__ratelimit(&rs) && dev)
				scrub_print_common_warning("generation error", dev, false,
						     sector_logical, sector_physical);
	}

	/* Update the device stats. */
	for (int i = 0; i < errors->nr_io_errors; i++)
		btrfs_dev_stat_inc_and_print(stripe->dev, BTRFS_DEV_STAT_READ_ERRS);
	for (int i = 0; i < errors->nr_csum_errors; i++)
		btrfs_dev_stat_inc_and_print(stripe->dev, BTRFS_DEV_STAT_CORRUPTION_ERRS);
	/* Generation mismatch error is based on each metadata, not each block. */
	for (int i = 0; i < errors->nr_meta_gen_errors;
	     i += (fs_info->nodesize >> fs_info->sectorsize_bits))
		btrfs_dev_stat_inc_and_print(stripe->dev, BTRFS_DEV_STAT_GENERATION_ERRS);

	spin_lock(&sctx->stat_lock);
	sctx->stat.data_extents_scrubbed += stripe->nr_data_extents;
	sctx->stat.tree_extents_scrubbed += stripe->nr_meta_extents;
	sctx->stat.data_bytes_scrubbed += nr_data_sectors << fs_info->sectorsize_bits;
	sctx->stat.tree_bytes_scrubbed += nr_meta_sectors << fs_info->sectorsize_bits;
	sctx->stat.no_csum += nr_nodatacsum_sectors;
	sctx->stat.read_errors += errors->nr_io_errors;
	sctx->stat.csum_errors += errors->nr_csum_errors;
	sctx->stat.verify_errors += errors->nr_meta_errors +
				    errors->nr_meta_gen_errors;
	sctx->stat.uncorrectable_errors +=
		bitmap_weight(&error_bitmap, stripe->nr_sectors) + nr_write_failed;
	sctx->stat.corrected_errors += nr_repaired_sectors;
	spin_unlock(&sctx->stat_lock);
}

static void scrub_write_sectors(struct scrub_ctx *sctx, struct scrub_stripe *stripe,
				unsigned long write_bitmap, bool dev_replace);

/*
 * The main entrance for all read related scrub work, including:
 *
 * - Wait for the initial read to finish
 * - Verify and locate any bad sectors
 * - Go through the remaining mirrors and try to read as large blocksize as
 *   possible
 * - Go through all mirrors (including the failed mirror) sector-by-sector
 * - Submit writeback for repaired sectors
 *
 * Writeback for dev-replace does not happen here, it needs extra
 * synchronization for zoned devices.
 */
/*
 * Mark the sectors of a data column that the write-intent log proved stale and
 * that nothing else can check.
 *
 * Runs after scrub_verify_one_stripe(), which is the whole point: for a sector
 * without a checksum that function clears the error bit rather than leaving it
 * set, because on its own it has nothing to verify against.  The log does.
 *
 * A sector holding metadata is left alone even without a data checksum: a tree
 * block carries its own header and generation, scrub_verify_one_metadata()
 * checks them, and that is the stronger evidence.  Likewise a sector that has a
 * checksum -- if it is really stale the checksum has already failed and the
 * repair below rebuilds it anyway.
 *
 * What is left is exactly the case this record exists for.
 *
 * Returns the sectors it marked.  Verification never sets those bits again, so
 * they have to be put back after every re-read of this mirror; see
 * scrub_verify_repair_read().
 */
static unsigned long scrub_mark_wib_stale_sectors(struct scrub_stripe *stripe)
{
	const unsigned long has_extent = scrub_bitmap_read_has_extent(stripe);
	const unsigned long is_metadata = scrub_bitmap_read_is_metadata(stripe);
	unsigned long marked = 0;
	int sector_nr;
	int nr_marked = 0;

	for_each_set_bit(sector_nr, &has_extent, stripe->nr_sectors) {
		if (test_bit(sector_nr, &is_metadata))
			continue;
		if (stripe->sectors[sector_nr].csum)
			continue;
		scrub_bitmap_set_bit_error(stripe, sector_nr);
		__set_bit(sector_nr, &marked);
		nr_marked++;
	}
	if (nr_marked)
		btrfs_warn_rl(stripe->bg->fs_info,
"scrub: rebuilding %d sector(s) at %llu from the parity: the write-intent log records their last write as not having reached the disk, and without a checksum nothing else can tell",
			      nr_marked, stripe->logical);
	return marked;
}

/*
 * Testing only: let a re-read of a stripe's own mirror clear the errors that
 * scrub_mark_wib_stale_sectors() forced, as scrub_stripe_read_repair_worker()
 * did before.  The negative control for uml/forced_stale.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool reread_trusts_stale;
module_param_named(raid56_scrub_reread_trusts_stale, reread_trusts_stale, bool, 0644);
MODULE_PARM_DESC(raid56_scrub_reread_trusts_stale,
		 "Let scrub's last-resort re-read of a data column the write-intent log names stale clear the errors the log forced on it (testing only: restores a known defect)");
#else
static const bool reread_trusts_stale;
#endif

/*
 * Verify what a repair read of @mirror has just put in the buffer for the
 * sectors in @reread, then put back the errors that no read of the stripe's
 * own mirror can cure.
 *
 * @forced are the sectors whose error bit was set not for anything found in
 * their bytes but because something else knows the copy on @stripe->mirror_num
 * is wrong: today the write-intent log, through
 * scrub_mark_wib_stale_sectors().  scrub_verify_one_sector() knows nothing of
 * that.  For a sector without a checksum it clears the error bit of whatever it
 * is handed -- "we have no other choice but to trust it".
 *
 * For a read of another mirror that is the point: on RAID5/6 it is the rebuild
 * the forcing asked for, and its bytes are not the ones the log condemned.  But
 * the last-resort pass of scrub_stripe_read_repair_worker() re-reads every
 * mirror, the stripe's own first, and for a sector no rebuild could supply --
 * the parity or a sibling column did not read -- that re-read puts the
 * condemned bytes straight back into the buffer.  They used to verify clean,
 * leave the error bitmap, be written back over themselves and be counted as
 * repaired; on the RAID5/6 parity path the parity was then regenerated from
 * them and the record retired.  The acknowledged value, which only the parity
 * still held, was gone, and nothing said so.
 *
 * Kept in error, the sector is refused instead: nothing is written from it,
 * scrub_raid56_parity_stripe() reports the full stripe unrepaired and keeps the
 * record (which raid56_health keeps counting), and a later scrub rebuilds it
 * once the parity reads again.
 *
 * Only the sectors @reread covers: one an earlier mirror already rebuilt was
 * not read again, still holds the rebuild, and is good.
 */
static void scrub_verify_repair_read(struct scrub_stripe *stripe, int mirror,
				     unsigned long reread, unsigned long forced)
{
	unsigned long again;
	int sector_nr;

	scrub_verify_one_stripe(stripe, reread);
	if (mirror != stripe->mirror_num || READ_ONCE(reread_trusts_stale))
		return;
	if (!bitmap_and(&again, &forced, &reread, stripe->nr_sectors))
		return;
	for_each_set_bit(sector_nr, &again, stripe->nr_sectors)
		scrub_bitmap_set_bit_error(stripe, sector_nr);
	btrfs_warn_rl(stripe->bg->fs_info,
"scrub: %d sector(s) at %llu re-read from mirror %d still hold what the write-intent log records as stale; keeping them in error rather than accepting them",
		      bitmap_weight(&again, stripe->nr_sectors), stripe->logical,
		      mirror);
}

/*
 * Note the sectors this mirror "repaired" that nothing can vouch for.
 *
 * On RAID5/6 any mirror above the first is a reconstruction from the parity,
 * not another copy.  scrub_verify_one_sector() clears the error bit of a
 * sector with no checksum unconditionally -- "we have no other choice but to
 * trust it" -- so a reconstruction of unchecksummed data is declared good
 * without anything having checked it, lands in @repaired, and is written back
 * over the sector it was meant to fix.
 *
 * If the parity was stale, or a sibling column of the same vertical stripe
 * was, that reconstruction is a value nothing ever committed, and persisting
 * it destroys the last copy of what was acknowledged.  The read path already
 * refuses exactly this (see repair_read_is_reconstruction() and the warning
 * beside it); scrub did the opposite.
 *
 * Metadata is exempt because a tree block carries its own header and
 * generation, which is proof of the same kind as a checksum.  The caller
 * exempts a stripe the write-intent log authorised (@wib_rebuild): there the
 * log names the member and the budget was checked, so the rebuild is proved
 * rather than guessed, and writing it back is the repair.
 */
static void scrub_note_unprovable(struct scrub_stripe *stripe,
				  unsigned long *unprovable,
				  const unsigned long old_error_bitmap)
{
	const unsigned long now_error = scrub_bitmap_read_error(stripe);
	const unsigned long is_metadata = scrub_bitmap_read_is_metadata(stripe);
	int sector_nr;

	for_each_set_bit(sector_nr, &old_error_bitmap, stripe->nr_sectors) {
		/* Still bad, so this mirror did not claim to have fixed it. */
		if (test_bit(sector_nr, &now_error))
			continue;
		if (stripe->sectors[sector_nr].csum)
			continue;
		if (test_bit(sector_nr, &is_metadata))
			continue;
		set_bit(sector_nr, unprovable);
	}
}

static void scrub_stripe_read_repair_worker(struct work_struct *work)
{
	struct scrub_stripe *stripe = container_of(work, struct scrub_stripe, work);
	struct scrub_ctx *sctx = stripe->sctx;
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct scrub_error_records errors = { 0 };
	int num_copies = btrfs_num_copies(fs_info, stripe->bg->start,
					  stripe->bg->length);
	unsigned long repaired;
	unsigned long error;
	unsigned long unprovable;
	/*
	 * Errors set from outside verification, which a re-read of this
	 * mirror must not clear: see scrub_verify_repair_read().  Anything
	 * that marks a sector bad for what is known about it rather than for
	 * what its bytes say belongs here.
	 */
	unsigned long forced = 0;
	/*
	 * On RAID5/6 every mirror above the first is a reconstruction from the
	 * parity rather than another copy of the bytes.
	 */
	const bool reconstructs = stripe->bg->flags & BTRFS_BLOCK_GROUP_RAID56_MASK;
	int sector_nr;
	int mirror;
	int i;

	ASSERT(stripe->mirror_num >= 1, "stripe->mirror_num=%d", stripe->mirror_num);

	bitmap_zero(&unprovable, stripe->nr_sectors);
	wait_scrub_stripe_io(stripe);
	scrub_verify_one_stripe(stripe, scrub_bitmap_read_has_extent(stripe));
	if (unlikely(stripe->wib_rebuild))
		forced = scrub_mark_wib_stale_sectors(stripe);
	/* Save the initial failed bitmap for later repair and report usage. */
	errors.init_error_bitmap = scrub_bitmap_read_error(stripe);
	errors.nr_io_errors = scrub_bitmap_weight_io_error(stripe);
	errors.nr_csum_errors = scrub_bitmap_weight_csum_error(stripe);
	errors.nr_meta_errors = scrub_bitmap_weight_meta_error(stripe);
	errors.nr_meta_gen_errors = scrub_bitmap_weight_meta_gen_error(stripe);

	if (bitmap_empty(&errors.init_error_bitmap, stripe->nr_sectors))
		goto out;

	/*
	 * Try all remaining mirrors.
	 *
	 * Here we still try to read as large block as possible, as this is
	 * faster and we have extra safety nets to rely on.
	 */
	for (mirror = calc_next_mirror(stripe->mirror_num, num_copies);
	     mirror != stripe->mirror_num;
	     mirror = calc_next_mirror(mirror, num_copies)) {
		const unsigned long old_error_bitmap = scrub_bitmap_read_error(stripe);

		scrub_stripe_submit_repair_read(stripe, mirror,
						BTRFS_STRIPE_LEN, false);
		wait_scrub_stripe_io(stripe);
		scrub_verify_repair_read(stripe, mirror, old_error_bitmap, forced);
		if (reconstructs && mirror > 1 && !stripe->wib_rebuild &&
		    !btrfs_raid56_scrub_trusts_rebuild())
			scrub_note_unprovable(stripe, &unprovable,
					      old_error_bitmap);
		if (scrub_bitmap_empty_error(stripe))
			goto out;
	}

	/*
	 * Last safety net, try re-checking all mirrors, including the failed
	 * one, sector-by-sector.
	 *
	 * As if one sector failed the drive's internal csum, the whole read
	 * containing the offending sector would be marked as error.
	 * Thus here we do sector-by-sector read.
	 *
	 * This can be slow, thus we only try it as the last resort.
	 *
	 * "Including the failed one" is a re-read of this stripe's own mirror,
	 * which for a sector the write-intent log names stale brings back the
	 * very bytes it condemned; scrub_verify_repair_read() keeps those in
	 * error.
	 */

	for (i = 0, mirror = stripe->mirror_num;
	     i < num_copies;
	     i++, mirror = calc_next_mirror(mirror, num_copies)) {
		const unsigned long old_error_bitmap = scrub_bitmap_read_error(stripe);

		scrub_stripe_submit_repair_read(stripe, mirror,
						fs_info->sectorsize, true);
		wait_scrub_stripe_io(stripe);
		scrub_verify_repair_read(stripe, mirror, old_error_bitmap, forced);
		if (reconstructs && mirror > 1 && !stripe->wib_rebuild &&
		    !btrfs_raid56_scrub_trusts_rebuild())
			scrub_note_unprovable(stripe, &unprovable,
					      old_error_bitmap);
		if (scrub_bitmap_empty_error(stripe))
			goto out;
	}
out:
	/*
	 * Put back the error bits of every sector that was only ever "repaired"
	 * by a reconstruction nothing could check.  They are excluded from
	 * @repaired below, so the guess is not written to the disk, and they
	 * are reported as still in error, which is what they are: the sector on
	 * disk is unreadable and the replacement is unproven.
	 */
	if (!bitmap_empty(&unprovable, stripe->nr_sectors)) {
		for_each_set_bit(sector_nr, &unprovable, stripe->nr_sectors)
			scrub_bitmap_set_bit_error(stripe, sector_nr);
		btrfs_warn_rl(fs_info,
"scrub: %d sector(s) at %llu were rebuilt from the parity but not written back: they have no checksum and the write-intent log does not name the member to blame, so nothing can tell a correct rebuild from a guess",
			      bitmap_weight(&unprovable, stripe->nr_sectors),
			      stripe->logical);
	}
	error = scrub_bitmap_read_error(stripe);
	/*
	 * Submit the repaired sectors.  For zoned case, we cannot do repair
	 * in-place, but queue the bg to be relocated.
	 */
	bitmap_andnot(&repaired, &errors.init_error_bitmap, &error,
		      stripe->nr_sectors);
	if (!sctx->readonly && !bitmap_empty(&repaired, stripe->nr_sectors)) {
		if (btrfs_is_zoned(fs_info)) {
			btrfs_repair_one_zone(fs_info, sctx->stripes[0].bg->start);
		} else {
			scrub_write_sectors(sctx, stripe, repaired, false);
			wait_scrub_stripe_io(stripe);
		}
	}

	scrub_stripe_report_errors(sctx, stripe, &errors);
	set_bit(SCRUB_STRIPE_FLAG_REPAIR_DONE, &stripe->state);
	wake_up(&stripe->repair_wait);
}

static void scrub_read_endio(struct btrfs_bio *bbio)
{
	struct scrub_stripe *stripe = bbio->private;

	scrub_read_endio_common(bbio);

	if (atomic_dec_and_test(&stripe->pending_io)) {
		wake_up(&stripe->io_wait);
		INIT_WORK(&stripe->work, scrub_stripe_read_repair_worker);
		queue_work(stripe->bg->fs_info->scrub_workers, &stripe->work);
	}
}

static void scrub_write_endio(struct btrfs_bio *bbio)
{
	struct scrub_stripe *stripe = bbio->private;
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	unsigned int sector_nr = calc_sector_number(bbio);
	const u32 bio_size = bio_get_size(&bbio->bio);

	if (bbio->bio.bi_status) {
		unsigned long flags;

		spin_lock_irqsave(&stripe->write_error_lock, flags);
		bitmap_set(&stripe->write_error_bitmap, sector_nr,
			   bio_size >> fs_info->sectorsize_bits);
		spin_unlock_irqrestore(&stripe->write_error_lock, flags);
		for (int i = 0; i < (bio_size >> fs_info->sectorsize_bits); i++)
			btrfs_dev_stat_inc_and_print(stripe->dev,
						     BTRFS_DEV_STAT_WRITE_ERRS);
	}
	bio_put(&bbio->bio);

	if (atomic_dec_and_test(&stripe->pending_io))
		wake_up(&stripe->io_wait);
}

static void scrub_submit_write_bio(struct scrub_ctx *sctx,
				   struct scrub_stripe *stripe,
				   struct btrfs_bio *bbio, bool dev_replace)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	u32 bio_len = bbio->bio.bi_iter.bi_size;
	u32 bio_off = (bbio->bio.bi_iter.bi_sector << SECTOR_SHIFT) -
		      stripe->logical;

	fill_writer_pointer_gap(sctx, stripe->physical + bio_off);
	atomic_inc(&stripe->pending_io);
	btrfs_submit_repair_write(bbio, stripe->mirror_num, dev_replace);
	if (!btrfs_is_zoned(fs_info))
		return;
	/*
	 * For zoned writeback, queue depth must be 1, thus we must wait for
	 * the write to finish before the next write.
	 */
	wait_scrub_stripe_io(stripe);

	/*
	 * And also need to update the write pointer if write finished
	 * successfully.
	 */
	if (!test_bit(bio_off >> fs_info->sectorsize_bits,
		      &stripe->write_error_bitmap))
		sctx->write_pointer += bio_len;
}

/*
 * Submit the write bio(s) for the sectors specified by @write_bitmap.
 *
 * Here we utilize btrfs_submit_repair_write(), which has some extra benefits:
 *
 * - Only needs logical bytenr and mirror_num
 *   Just like the scrub read path
 *
 * - Would only result in writes to the specified mirror
 *   Unlike the regular writeback path, which would write back to all stripes
 *
 * - Handle dev-replace and read-repair writeback differently
 */
static void scrub_write_sectors(struct scrub_ctx *sctx, struct scrub_stripe *stripe,
				unsigned long write_bitmap, bool dev_replace)
{
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	struct btrfs_bio *bbio = NULL;
	int sector_nr;

	for_each_set_bit(sector_nr, &write_bitmap, stripe->nr_sectors) {
		/*
		 * We should only writeback sectors covered by an extent --
		 * except to a replace target taking a RAID5/6 data column
		 * whole, where the free sectors are what the parity was
		 * computed over too.
		 */
		ASSERT(scrub_bitmap_test_bit_has_extent(stripe, sector_nr) ||
		       (dev_replace &&
			test_bit(SCRUB_STRIPE_FLAG_WHOLE_COLUMN, &stripe->state)));

		/* Cannot merge with previous sector, submit the current one. */
		if (bbio && sector_nr && !test_bit(sector_nr - 1, &write_bitmap)) {
			scrub_submit_write_bio(sctx, stripe, bbio, dev_replace);
			bbio = NULL;
		}
		if (!bbio)
			bbio = alloc_scrub_bbio(fs_info, REQ_OP_WRITE,
					stripe->logical + (sector_nr << fs_info->sectorsize_bits),
					scrub_write_endio, stripe);
		scrub_bio_add_sector(bbio, stripe, sector_nr);
	}
	if (bbio)
		scrub_submit_write_bio(sctx, stripe, bbio, dev_replace);
}

/*
 * Throttling of IO submission, bandwidth-limit based, the timeslice is 1
 * second.  Limit can be set via /sys/fs/UUID/devinfo/devid/scrub_speed_max.
 */
static void scrub_throttle_dev_io(struct scrub_ctx *sctx, struct btrfs_device *device,
				  unsigned int bio_size)
{
	const int time_slice = 1000;
	s64 delta;
	ktime_t now;
	u32 div;
	u64 bwlimit;

	bwlimit = READ_ONCE(device->scrub_speed_max);
	if (bwlimit == 0)
		return;

	/*
	 * Slice is divided into intervals when the IO is submitted, adjust by
	 * bwlimit and maximum of 64 intervals.
	 */
	div = clamp(bwlimit / (16 * 1024 * 1024), 1, 64);

	/* Start new epoch, set deadline */
	now = ktime_get();
	if (sctx->throttle_deadline == 0) {
		sctx->throttle_deadline = ktime_add_ms(now, time_slice / div);
		sctx->throttle_sent = 0;
	}

	/* Still in the time to send? */
	if (ktime_before(now, sctx->throttle_deadline)) {
		/* If current bio is within the limit, send it */
		sctx->throttle_sent += bio_size;
		if (sctx->throttle_sent <= div_u64(bwlimit, div))
			return;

		/* We're over the limit, sleep until the rest of the slice */
		delta = ktime_ms_delta(sctx->throttle_deadline, now);
	} else {
		/* New request after deadline, start new epoch */
		delta = 0;
	}

	if (delta) {
		long timeout;

		timeout = div_u64(delta * HZ, 1000);
		schedule_timeout_interruptible(timeout);
	}

	/* Next call will start the deadline period */
	sctx->throttle_deadline = 0;
}

/*
 * Given a physical address, this will calculate it's
 * logical offset. if this is a parity stripe, it will return
 * the most left data stripe's logical offset.
 *
 * return 0 if it is a data stripe, 1 means parity stripe.
 */
static int get_raid56_logic_offset(u64 physical, int num,
				   struct btrfs_chunk_map *map, u64 *offset,
				   u64 *stripe_start)
{
	int i;
	int j = 0;
	u64 last_offset;
	const int data_stripes = nr_data_stripes(map);

	last_offset = (physical - map->stripes[num].physical) * data_stripes;
	if (stripe_start)
		*stripe_start = last_offset;

	*offset = last_offset;
	for (i = 0; i < data_stripes; i++) {
		u32 stripe_nr;
		u32 stripe_index;
		u32 rot;

		*offset = last_offset + btrfs_stripe_nr_to_offset(i);

		stripe_nr = (u32)(*offset >> BTRFS_STRIPE_LEN_SHIFT) / data_stripes;

		/* Work out the disk rotation on this stripe-set */
		rot = stripe_nr % map->num_stripes;
		/* calculate which stripe this data locates */
		rot += i;
		stripe_index = rot % map->num_stripes;
		if (stripe_index == num)
			return 0;
		if (stripe_index < num)
			j++;
	}
	*offset = last_offset + btrfs_stripe_nr_to_offset(j);
	return 1;
}

/*
 * Return 0 if the extent item range covers any byte of the range.
 * Return <0 if the extent item is before @search_start.
 * Return >0 if the extent item is after @start_start + @search_len.
 */
static int compare_extent_item_range(struct btrfs_path *path,
				     u64 search_start, u64 search_len)
{
	struct btrfs_fs_info *fs_info = path->nodes[0]->fs_info;
	u64 len;
	struct btrfs_key key;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	ASSERT(key.type == BTRFS_EXTENT_ITEM_KEY ||
	       key.type == BTRFS_METADATA_ITEM_KEY, "key.type=%u", key.type);
	if (key.type == BTRFS_METADATA_ITEM_KEY)
		len = fs_info->nodesize;
	else
		len = key.offset;

	if (key.objectid + len <= search_start)
		return -1;
	if (key.objectid >= search_start + search_len)
		return 1;
	return 0;
}

/*
 * Locate one extent item which covers any byte in range
 * [@search_start, @search_start + @search_length)
 *
 * If the path is not initialized, we will initialize the search by doing
 * a btrfs_search_slot().
 * If the path is already initialized, we will use the path as the initial
 * slot, to avoid duplicated btrfs_search_slot() calls.
 *
 * NOTE: If an extent item starts before @search_start, we will still
 * return the extent item. This is for data extent crossing stripe boundary.
 *
 * Return 0 if we found such extent item, and @path will point to the extent item.
 * Return >0 if no such extent item can be found, and @path will be released.
 * Return <0 if hit fatal error, and @path will be released.
 */
static int find_first_extent_item(struct btrfs_root *extent_root,
				  struct btrfs_path *path,
				  u64 search_start, u64 search_len)
{
	struct btrfs_fs_info *fs_info = extent_root->fs_info;
	struct btrfs_key key;
	int ret;

	/* Continue using the existing path */
	if (path->nodes[0])
		goto search_forward;

	key.objectid = search_start;
	if (btrfs_fs_incompat(fs_info, SKINNY_METADATA))
		key.type = BTRFS_METADATA_ITEM_KEY;
	else
		key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0)
		return ret;
	if (unlikely(ret == 0)) {
		/*
		 * Key with offset -1 found, there would have to exist an extent
		 * item with such offset, but this is out of the valid range.
		 */
		btrfs_release_path(path);
		return -EUCLEAN;
	}

	/*
	 * Here we intentionally pass 0 as @min_objectid, as there could be
	 * an extent item starting before @search_start.
	 */
	ret = btrfs_previous_extent_item(extent_root, path, 0);
	if (ret < 0)
		return ret;
	/*
	 * No matter whether we have found an extent item, the next loop will
	 * properly do every check on the key.
	 */
search_forward:
	while (true) {
		btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
		if (key.objectid >= search_start + search_len)
			break;
		if (key.type != BTRFS_METADATA_ITEM_KEY &&
		    key.type != BTRFS_EXTENT_ITEM_KEY)
			goto next;

		ret = compare_extent_item_range(path, search_start, search_len);
		if (ret == 0)
			return ret;
		if (ret > 0)
			break;
next:
		ret = btrfs_next_item(extent_root, path);
		if (ret) {
			/* Either no more items or a fatal error. */
			btrfs_release_path(path);
			return ret;
		}
	}
	btrfs_release_path(path);
	return 1;
}

static void get_extent_info(struct btrfs_path *path, u64 *extent_start_ret,
			    u64 *size_ret, u64 *flags_ret, u64 *generation_ret)
{
	struct btrfs_key key;
	struct btrfs_extent_item *ei;

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);
	ASSERT(key.type == BTRFS_METADATA_ITEM_KEY ||
	       key.type == BTRFS_EXTENT_ITEM_KEY, "key.type=%u", key.type);
	*extent_start_ret = key.objectid;
	if (key.type == BTRFS_METADATA_ITEM_KEY)
		*size_ret = path->nodes[0]->fs_info->nodesize;
	else
		*size_ret = key.offset;
	ei = btrfs_item_ptr(path->nodes[0], path->slots[0], struct btrfs_extent_item);
	*flags_ret = btrfs_extent_flags(path->nodes[0], ei);
	*generation_ret = btrfs_extent_generation(path->nodes[0], ei);
}

static int sync_write_pointer_for_zoned(struct scrub_ctx *sctx, u64 logical,
					u64 physical, u64 physical_end)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	int ret = 0;

	if (!btrfs_is_zoned(fs_info))
		return 0;

	mutex_lock(&sctx->wr_lock);
	if (sctx->write_pointer < physical_end) {
		ret = btrfs_sync_zone_write_pointer(sctx->wr_tgtdev, logical,
						    physical,
						    sctx->write_pointer);
		if (ret)
			btrfs_err(fs_info, "scrub: zoned: failed to recover write pointer");
	}
	mutex_unlock(&sctx->wr_lock);
	btrfs_dev_clear_zone_empty(sctx->wr_tgtdev, physical);

	return ret;
}

static void fill_one_extent_info(struct btrfs_fs_info *fs_info,
				 struct scrub_stripe *stripe,
				 u64 extent_start, u64 extent_len,
				 u64 extent_flags, u64 extent_gen)
{
	for (u64 cur_logical = max(stripe->logical, extent_start);
	     cur_logical < min(stripe->logical + BTRFS_STRIPE_LEN,
			       extent_start + extent_len);
	     cur_logical += fs_info->sectorsize) {
		const int nr_sector = (cur_logical - stripe->logical) >>
				      fs_info->sectorsize_bits;
		struct scrub_sector_verification *sector =
						&stripe->sectors[nr_sector];

		scrub_bitmap_set_bit_has_extent(stripe, nr_sector);
		if (extent_flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
			scrub_bitmap_set_bit_is_metadata(stripe, nr_sector);
			sector->generation = extent_gen;
		}
	}
}

static void scrub_stripe_reset_bitmaps(struct scrub_stripe *stripe)
{
	ASSERT(stripe->nr_sectors);
	bitmap_zero(stripe->bitmaps, scrub_bitmap_nr_last * stripe->nr_sectors);
}

/*
 * Locate one stripe which has at least one extent in its range.
 *
 * Return 0 if found such stripe, and store its info into @stripe.
 * Return >0 if there is no such stripe in the specified range.
 * Return <0 for error.
 */
static int scrub_find_fill_first_stripe(struct btrfs_block_group *bg,
					struct btrfs_path *extent_path,
					struct btrfs_path *csum_path,
					struct btrfs_device *dev, u64 physical,
					int mirror_num, u64 logical_start,
					u32 logical_len,
					struct scrub_stripe *stripe)
{
	struct btrfs_fs_info *fs_info = bg->fs_info;
	struct btrfs_root *extent_root = btrfs_extent_root(fs_info, bg->start);
	struct btrfs_root *csum_root = btrfs_csum_root(fs_info, bg->start);
	const u64 logical_end = logical_start + logical_len;
	u64 cur_logical = logical_start;
	u64 stripe_end;
	u64 extent_start;
	u64 extent_len;
	u64 extent_flags;
	u64 extent_gen;
	int ret;

	if (unlikely(!extent_root || !csum_root)) {
		btrfs_err(fs_info, "scrub: no valid extent or csum root found");
		return -EUCLEAN;
	}
	memset(stripe->sectors, 0, sizeof(struct scrub_sector_verification) *
				   stripe->nr_sectors);
	scrub_stripe_reset_bitmaps(stripe);

	/* The range must be inside the bg. */
	ASSERT(logical_start >= bg->start && logical_end <= btrfs_block_group_end(bg),
	       "bg->start=%llu logical_start=%llu logical_end=%llu end=%llu",
	       bg->start, logical_start, logical_end, btrfs_block_group_end(bg));

	ret = find_first_extent_item(extent_root, extent_path, logical_start,
				     logical_len);
	/* Either error or not found. */
	if (ret)
		return ret;
	get_extent_info(extent_path, &extent_start, &extent_len, &extent_flags,
			&extent_gen);
	if (extent_flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
		stripe->nr_meta_extents++;
	if (extent_flags & BTRFS_EXTENT_FLAG_DATA)
		stripe->nr_data_extents++;
	cur_logical = max(extent_start, cur_logical);

	/*
	 * Round down to stripe boundary.
	 *
	 * The extra calculation against bg->start is to handle block groups
	 * whose logical bytenr is not BTRFS_STRIPE_LEN aligned.
	 */
	stripe->logical = round_down(cur_logical - bg->start, BTRFS_STRIPE_LEN) +
			  bg->start;
	stripe->physical = physical + stripe->logical - logical_start;
	stripe->dev = dev;
	stripe->bg = bg;
	stripe->mirror_num = mirror_num;
	stripe_end = stripe->logical + BTRFS_STRIPE_LEN - 1;

	/* Fill the first extent info into stripe->sectors[] array. */
	fill_one_extent_info(fs_info, stripe, extent_start, extent_len,
			     extent_flags, extent_gen);
	cur_logical = extent_start + extent_len;

	/* Fill the extent info for the remaining sectors. */
	while (cur_logical <= stripe_end) {
		ret = find_first_extent_item(extent_root, extent_path, cur_logical,
					     stripe_end - cur_logical + 1);
		if (ret < 0)
			return ret;
		if (ret > 0) {
			ret = 0;
			break;
		}
		get_extent_info(extent_path, &extent_start, &extent_len,
				&extent_flags, &extent_gen);
		if (extent_flags & BTRFS_EXTENT_FLAG_TREE_BLOCK)
			stripe->nr_meta_extents++;
		if (extent_flags & BTRFS_EXTENT_FLAG_DATA)
			stripe->nr_data_extents++;
		fill_one_extent_info(fs_info, stripe, extent_start, extent_len,
				     extent_flags, extent_gen);
		cur_logical = extent_start + extent_len;
	}

	/* Now fill the data csum. */
	if (bg->flags & BTRFS_BLOCK_GROUP_DATA) {
		int sector_nr;
		unsigned long csum_bitmap = 0;

		/* Csum space should have already been allocated. */
		ASSERT(stripe->csums);

		/*
		 * Our csum bitmap should be large enough, as BTRFS_STRIPE_LEN
		 * should contain at most 16 sectors.
		 */
		ASSERT(BITS_PER_LONG >= BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits);

		ret = btrfs_lookup_csums_bitmap(csum_root, csum_path,
						stripe->logical, stripe_end,
						stripe->csums, &csum_bitmap);
		if (ret < 0)
			return ret;
		if (ret > 0)
			ret = 0;

		for_each_set_bit(sector_nr, &csum_bitmap, stripe->nr_sectors) {
			stripe->sectors[sector_nr].csum = stripe->csums +
				sector_nr * fs_info->csum_size;
		}
	}
	set_bit(SCRUB_STRIPE_FLAG_INITIALIZED, &stripe->state);

	return ret;
}

static void scrub_reset_stripe(struct scrub_stripe *stripe)
{
	scrub_stripe_reset_bitmaps(stripe);
	/*
	 * Not one of stripe->bitmaps, so scrub_stripe_reset_bitmaps() does not
	 * cover it: scrub_write_endio() sets it when a repair write fails, and
	 * a reused stripe would carry that into the next thing it describes.
	 */
	stripe->write_error_bitmap = 0;

	stripe->nr_meta_extents = 0;
	stripe->nr_data_extents = 0;
	stripe->state = 0;
	stripe->wib_rebuild = false;

	for (int i = 0; i < stripe->nr_sectors; i++) {
		stripe->sectors[i].csum = NULL;
		stripe->sectors[i].generation = 0;
	}
}

static u32 stripe_length(const struct scrub_stripe *stripe)
{
	ASSERT(stripe->bg);

	return min(BTRFS_STRIPE_LEN,
		   stripe->bg->start + stripe->bg->length - stripe->logical);
}

static void scrub_submit_extent_sector_read(struct scrub_stripe *stripe)
{
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	struct btrfs_bio *bbio = NULL;
	unsigned int nr_sectors = stripe_length(stripe) >> fs_info->sectorsize_bits;
	const unsigned long has_extent = scrub_bitmap_read_has_extent(stripe);
	u64 stripe_len = BTRFS_STRIPE_LEN;
	int mirror = stripe->mirror_num;
	int i;

	atomic_inc(&stripe->pending_io);

	for_each_set_bit(i, &has_extent, stripe->nr_sectors) {
		/* We're beyond the chunk boundary, no need to read anymore. */
		if (i >= nr_sectors)
			break;

		/* The current sector cannot be merged, submit the bio. */
		if (bbio &&
		    ((i > 0 && !test_bit(i - 1, &has_extent)) ||
		     bbio->bio.bi_iter.bi_size >= stripe_len)) {
			ASSERT(bbio->bio.bi_iter.bi_size);
			atomic_inc(&stripe->pending_io);
			btrfs_submit_bbio(bbio, mirror);
			bbio = NULL;
		}

		if (!bbio) {
			struct btrfs_io_stripe io_stripe = {};
			struct btrfs_io_context *bioc = NULL;
			const u64 logical = stripe->logical +
					    (i << fs_info->sectorsize_bits);
			int ret;

			io_stripe.rst_search_commit_root = true;
			stripe_len = (nr_sectors - i) << fs_info->sectorsize_bits;
			/*
			 * For RST cases, we need to manually split the bbio to
			 * follow the RST boundary.
			 */
			ret = btrfs_map_block(fs_info, BTRFS_MAP_READ, logical,
					      &stripe_len, &bioc, &io_stripe, &mirror);
			btrfs_put_bioc(bioc);
			if (ret < 0) {
				if (ret != -ENODATA) {
					/*
					 * Earlier btrfs_get_raid_extent_offset()
					 * returned -ENODATA, which means there's
					 * no entry for the corresponding range
					 * in the stripe tree.  But if it's in
					 * the extent tree, then it's a preallocated
					 * extent and not an error.
					 */
					scrub_bitmap_set_bit_io_error(stripe, i);
					scrub_bitmap_set_bit_error(stripe, i);
				}
				continue;
			}

			bbio = alloc_scrub_bbio(fs_info, REQ_OP_READ,
						logical, scrub_read_endio, stripe);
		}

		scrub_bio_add_sector(bbio, stripe, i);
	}

	if (bbio) {
		ASSERT(bbio->bio.bi_iter.bi_size);
		atomic_inc(&stripe->pending_io);
		btrfs_submit_bbio(bbio, mirror);
	}

	if (atomic_dec_and_test(&stripe->pending_io)) {
		wake_up(&stripe->io_wait);
		INIT_WORK(&stripe->work, scrub_stripe_read_repair_worker);
		queue_work(stripe->bg->fs_info->scrub_workers, &stripe->work);
	}
}

/*
 * The initial read of @stripe from @mirror when it is a device replace's
 * rebuild of a RAID5/6 data column: one bio per run of sectors that hold an
 * extent or that do not, those marked (scrub_replace_splits_free()).
 */
static void scrub_submit_initial_read_split(struct scrub_stripe *stripe, int mirror,
					    unsigned int nr_sectors)
{
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	const unsigned long has_extent = scrub_bitmap_read_has_extent(stripe);
	struct btrfs_bio *bbio = NULL;

	/* Held until every bio is submitted: the last to end runs the repair. */
	atomic_inc(&stripe->pending_io);
	for (unsigned int cur = 0; cur < nr_sectors; cur++) {
		const bool reads_free = !test_bit(cur, &has_extent);

		if (bbio && bbio->scrub_reads_free != reads_free) {
			atomic_inc(&stripe->pending_io);
			btrfs_submit_bbio(bbio, mirror);
			bbio = NULL;
		}
		if (!bbio) {
			bbio = alloc_scrub_bbio(fs_info, REQ_OP_READ,
						stripe->logical +
						(cur << fs_info->sectorsize_bits),
						scrub_read_endio, stripe);
			bbio->scrub_reads_free = reads_free;
		}
		scrub_bio_add_sector(bbio, stripe, cur);
	}
	if (bbio) {
		atomic_inc(&stripe->pending_io);
		btrfs_submit_bbio(bbio, mirror);
	}
	if (atomic_dec_and_test(&stripe->pending_io)) {
		wake_up(&stripe->io_wait);
		INIT_WORK(&stripe->work, scrub_stripe_read_repair_worker);
		queue_work(fs_info->scrub_workers, &stripe->work);
	}
}

static void scrub_submit_initial_read(struct scrub_ctx *sctx,
				      struct scrub_stripe *stripe)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct btrfs_bio *bbio;
	unsigned int nr_sectors = stripe_length(stripe) >> fs_info->sectorsize_bits;
	int mirror = stripe->mirror_num;

	ASSERT(stripe->bg);
	ASSERT(stripe->mirror_num > 0);
	ASSERT(test_bit(SCRUB_STRIPE_FLAG_INITIALIZED, &stripe->state));

	if (btrfs_need_stripe_tree_update(fs_info, stripe->bg->flags)) {
		scrub_submit_extent_sector_read(stripe);
		return;
	}

	/*
	 * For dev-replace, either user asks to avoid the source dev, or
	 * the device is missing, we try the next mirror instead.
	 */
	if (sctx->is_dev_replace &&
	    (fs_info->dev_replace.cont_reading_from_srcdev_mode ==
	     BTRFS_DEV_REPLACE_ITEM_CONT_READING_FROM_SRCDEV_MODE_AVOID ||
	     !stripe->dev->bdev)) {
		int num_copies = btrfs_num_copies(fs_info, stripe->bg->start,
						  stripe->bg->length);

		mirror = calc_next_mirror(mirror, num_copies);
	}
	if (scrub_replace_splits_free(stripe, mirror)) {
		scrub_submit_initial_read_split(stripe, mirror, nr_sectors);
		return;
	}

	bbio = alloc_scrub_bbio(fs_info, REQ_OP_READ,
				stripe->logical, scrub_read_endio, stripe);
	/* Read the whole range inside the chunk boundary. */
	for (unsigned int cur = 0; cur < nr_sectors; cur++)
		scrub_bio_add_sector(bbio, stripe, cur);
	atomic_inc(&stripe->pending_io);
	btrfs_submit_bbio(bbio, mirror);
}

static bool stripe_has_metadata_error(struct scrub_stripe *stripe)
{
	const unsigned long error = scrub_bitmap_read_error(stripe);
	int i;

	for_each_set_bit(i, &error, stripe->nr_sectors) {
		if (scrub_bitmap_test_bit_is_metadata(stripe, i)) {
			struct btrfs_fs_info *fs_info = stripe->bg->fs_info;

			btrfs_err(fs_info,
		    "scrub: stripe %llu has unrepaired metadata sector at logical %llu",
				  stripe->logical,
				  stripe->logical + (i << fs_info->sectorsize_bits));
			return true;
		}
	}
	return false;
}

static void submit_initial_group_read(struct scrub_ctx *sctx,
				      unsigned int first_slot,
				      unsigned int nr_stripes)
{
	struct blk_plug plug;

	ASSERT(first_slot < SCRUB_TOTAL_STRIPES);
	ASSERT(first_slot + nr_stripes <= SCRUB_TOTAL_STRIPES);

	scrub_throttle_dev_io(sctx, sctx->stripes[0].dev,
			      btrfs_stripe_nr_to_offset(nr_stripes));
	blk_start_plug(&plug);
	for (int i = 0; i < nr_stripes; i++) {
		struct scrub_stripe *stripe = &sctx->stripes[first_slot + i];

		/* Those stripes should be initialized. */
		ASSERT(test_bit(SCRUB_STRIPE_FLAG_INITIALIZED, &stripe->state));
		scrub_submit_initial_read(sctx, stripe);
	}
	blk_finish_plug(&plug);
}

/*
 * Testing only: let a RAID5/6 device replace copy only the sectors an extent
 * covers of each data column, as it did before, and count nothing it could not
 * copy.  The negative control for uml/replace_whole_column.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool replace_extents_only;
module_param_named(raid56_wf_replace_extents_only, replace_extents_only, bool, 0644);
MODULE_PARM_DESC(raid56_wf_replace_extents_only,
		 "Let a RAID5/6 device replace copy only the sectors an extent covers, leaving the rest of each data column as whatever the new device held (testing only: restores a known defect)");
#else
static const bool replace_extents_only;
#endif

/*
 * Testing only: the negative controls for uml/replace_source_data.sh.
 *
 * raid56_wf_replace_rebuilds_unchecked lets a RAID5/6 device replace rebuild
 * the data without a checksum that the source returned, in a full stripe the
 * write-intent log does not record, instead of copying it, and rebuild what
 * the source did not return before asking the source again; see
 * scrub_replace_copy_column().
 *
 * raid56_wf_replace_keeps_marks lets the stale marks a replace makes for what
 * it could not copy take effect at once, while the source still serves the
 * column, and outlive a replace that does not finish; see
 * scrub_replace_record_lost().
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool replace_rebuilds_unchecked;
module_param_named(raid56_wf_replace_rebuilds_unchecked, replace_rebuilds_unchecked,
		   bool, 0644);
MODULE_PARM_DESC(raid56_wf_replace_rebuilds_unchecked,
		 "Let a RAID5/6 device replace put a rebuild from the parity on the new device in place of data without a checksum that the old device returned (testing only: restores a known defect)");
static bool replace_keeps_marks;
module_param_named(raid56_wf_replace_keeps_marks, replace_keeps_marks, bool, 0644);
MODULE_PARM_DESC(raid56_wf_replace_keeps_marks,
		 "Let the stale marks a RAID5/6 device replace makes for sectors it cannot copy apply to the old device at once and outlive a replace that does not finish (testing only: restores a known defect)");
#else
static const bool replace_rebuilds_unchecked;
static const bool replace_keeps_marks;
#endif

/*
 * Testing only: raid56_wf_replace_abort_unlatched=1 raises no
 * replace_aborted alert when a replace is aborted because it could not record
 * what it could neither copy nor rebuild, as before the alert existed: with
 * nothing recorded the state goes back to ok as soon as the alert work runs.
 * The negative control for uml/replace_abort.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool replace_abort_unlatched;
module_param_named(raid56_wf_replace_abort_unlatched, replace_abort_unlatched, bool, 0644);
MODULE_PARM_DESC(raid56_wf_replace_abort_unlatched,
		 "Raise no latched alert when a RAID5/6 device replace is aborted because it could not record what it could not copy, so the health goes back to ok (testing only: restores a known defect)");
#else
static const bool replace_abort_unlatched;
#endif

/*
 * A replace is aborted because it could not record what it could neither copy
 * nor rebuild (@why says why).  Nothing may be recorded then, and no record
 * would keep the health failing until the admin has seen it: raise the event
 * that does.
 */
static void scrub_replace_aborted(struct btrfs_fs_info *fs_info, u64 full_stripe_start)
{
	if (!READ_ONCE(replace_abort_unlatched))
		btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_REPLACE_ABORTED,
				   full_stripe_start, NULL, 0);
}

/*
 * Does a device replace of @bg copy whole RAID5/6 data columns?
 *
 * The parity of a full stripe describes every sector of every data column,
 * allocated or not: a read-modify-write reads the sectors of the columns it
 * does not write, free ones included, and computes the parity over all of them
 * (rmw_read_wait_recover(), generate_pq_vertical()).  So a free sector is not
 * nothing.  It is one of the terms the parity needs to give any OTHER column
 * of its row back.
 *
 * The replace used to copy only the sectors an extent covers, and the target
 * kept whatever it held everywhere else -- the free sectors of a column, and
 * the whole column wherever the source's column of a full stripe held no
 * extent while its neighbours did.  Nothing said so.  The array had lost its
 * redundancy on those rows, and the first time another device went missing,
 * every sector rebuilt against one of those holes was wrong; where the data
 * has no checksum it was returned as the file's content.  Data without a
 * checksum was only ever copied as the source held it, whatever the parity
 * said.  tools/testing/btrfs/uml/replace_whole_column.sh reads both back.
 *
 * Only RAID5/6: the other profiles copy mirrors, which nothing is computed
 * from.
 */
static bool scrub_replace_copies_column(const struct scrub_ctx *sctx,
					const struct btrfs_block_group *bg)
{
	return sctx->is_dev_replace &&
	       (bg->flags & BTRFS_BLOCK_GROUP_RAID56_MASK) &&
	       !READ_ONCE(replace_extents_only);
}

/* Make @bitmap the set of sectors the next repair read of @stripe reads. */
static void scrub_stripe_set_error(struct scrub_stripe *stripe, unsigned long bitmap)
{
	int sector_nr;

	scrub_bitmap_clear_error(stripe, 0, stripe->nr_sectors);
	for_each_set_bit(sector_nr, &bitmap, stripe->nr_sectors)
		scrub_bitmap_set_bit_error(stripe, sector_nr);
}

/*
 * Read the sectors @want of @stripe from @mirror into its buffer, and return
 * the ones that could not be had.
 *
 * A bio fails as a whole, and on RAID5/6 a rebuild that has to refuse one row
 * fails every sector it was asked for, so what failed is asked for again one
 * sector at a time to find out which of them really cannot be read.
 */
static unsigned long scrub_replace_read(struct scrub_stripe *stripe,
					unsigned long want, int mirror)
{
	struct btrfs_fs_info *fs_info = stripe->bg->fs_info;
	unsigned long io_error;
	unsigned long failed;

	scrub_stripe_set_error(stripe, want);
	scrub_stripe_submit_repair_read(stripe, mirror, BTRFS_STRIPE_LEN, false);
	wait_scrub_stripe_io(stripe);
	io_error = scrub_bitmap_read_io_error(stripe);
	bitmap_and(&failed, &want, &io_error, stripe->nr_sectors);
	if (bitmap_weight(&failed, stripe->nr_sectors) <= 1)
		return failed;

	scrub_stripe_set_error(stripe, failed);
	scrub_stripe_submit_repair_read(stripe, mirror, fs_info->sectorsize, true);
	io_error = scrub_bitmap_read_io_error(stripe);
	bitmap_and(&failed, &failed, &io_error, stripe->nr_sectors);
	return failed;
}

/*
 * Open a record for the full stripe at @full_stripe_start that a member of it
 * can then be named in.  Returns NULL, or why no record can be kept that
 * outlives this mount.
 */
static const char *scrub_replace_add_record(struct btrfs_fs_info *fs_info,
					    u64 full_stripe_start, int nr_data)
{
	if (!btrfs_wib_persisting(fs_info))
		return "the write-intent log is not enabled";
	/* See btrfs_wib_stripe_state(): no one would be told. */
	if (nr_data > 64)
		return "the write-intent log cannot describe a full stripe that wide";
	if (btrfs_wib_try_add_sticky(fs_info, full_stripe_start,
				     btrfs_stripe_nr_to_offset(nr_data)) < 0)
		return "the write-intent log is full";
	return NULL;
}

/*
 * The sectors @lost of @stripe's column can be neither copied nor rebuilt.
 * Count them, raise the alert, and record the target's column stale before the
 * zeros that stand in for them are written.
 *
 * Stale, and only the column.  What was committed there is still what the
 * parity and the other columns of the full stripe describe -- nothing about
 * them went wrong -- so a read of the column rebuilds from them instead of
 * believing the zeros: it fails with EIO while the device whose read failed is
 * still failing, and returns the right data once it is back.  A later scrub
 * (scrub_raid56_plan_wib()) or read-modify-write (rmw_prepare_repair())
 * rebuilds the column the same way, writes it and retires the record.
 * Recording the parity bad as well would forbid the rebuild the one thing that
 * still holds the data: on RAID5 the column could never be read again, and on
 * RAID6 every other column of the stripe would lose its rebuild along with it.
 * That is also how the model behind this copy records what it cannot copy
 * (map-recovery's recovery_model.py, s1-whole-record: 0 wrong reads).
 *
 * The whole column, too, because the log has no smaller unit (one bit per
 * BTRFS_STRIPE_LEN).  Once the target serves it, every sector of the column
 * without a checksum is read through a rebuild (btrfs_check_read_bio()) --
 * also the ones this copy got right, which then fail with EIO wherever that
 * rebuild cannot be done, typically for as long as the sibling or parity that
 * made these sectors uncopyable stays unreadable in their rows too.  A record
 * of only the lost rows would spare them, and the log cannot express one;
 * recording less than the column would let the zeros be read as data.  The
 * next scrub or write of the stripe rebuilds what it can of the column and
 * retires the record once all of it is back (scrub_raid56_plan_wib(),
 * rmw_prepare_repair()).
 *
 * The mark describes the target, not the source.  Until the replace finishes,
 * the source goes on serving the column, so the mark is the replace's own
 * until then (btrfs_wib_replace_mark_stale()): the source's good sectors are
 * read as they were, its siblings still rebuild from it, and a replace that is
 * cancelled or fails takes the mark back with its target
 * (btrfs_wib_replace_end()) instead of leaving the source's column condemned
 * -- to be rebuilt from the parity by the next scrub or write, and rebuilt
 * rather than copied by the next replace.
 *
 * The record is made before any zero is written.  If it cannot be made -- the
 * log is full, or it is not being persisted and the record would not outlive
 * this mount -- nothing is written and the replace is aborted: the source
 * stays in the filesystem and nothing is lost that was not lost already.
 *
 * Returns 0, or -EIO to abort the replace.
 */
static int scrub_replace_record_lost(struct scrub_ctx *sctx,
				     struct scrub_stripe *stripe,
				     unsigned long lost)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	const unsigned long has_extent = scrub_bitmap_read_has_extent(stripe);
	const int nr = bitmap_weight(&lost, stripe->nr_sectors);
	const char *why = NULL;
	unsigned long data;
	int nr_data;

	bitmap_and(&data, &lost, &has_extent, stripe->nr_sectors);
	nr_data = bitmap_weight(&data, stripe->nr_sectors);
	atomic64_add(nr, &fs_info->dev_replace.num_uncorrectable_read_errors);

	why = scrub_replace_add_record(fs_info, stripe->raid56_full_stripe,
				       stripe->raid56_nr_data);
	/*
	 * Marking can make the log shrink to fit (the first stale bit halves
	 * what a block holds); a record that did not survive that is no
	 * record.
	 */
	if (!why &&
	    !btrfs_wib_replace_mark_stale(fs_info, stripe->logical,
					  !READ_ONCE(replace_keeps_marks)))
		why = "the write-intent log could not keep the record";
	btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_REPLACE_LOST,
			   stripe->raid56_full_stripe, NULL, 0);
	if (why) {
		btrfs_err(fs_info,
"scrub: device replace can neither copy nor rebuild %d sector(s) at logical %llu (%d holding data), and cannot record them because %s; aborting the replace rather than leave zeros on the target that would read back as data",
			  nr, stripe->logical, nr_data, why);
		scrub_replace_aborted(fs_info, stripe->raid56_full_stripe);
		return -EIO;
	}
	btrfs_err_rl(fs_info,
"scrub: device replace could neither copy nor rebuild %d sector(s) at logical %llu (%d holding data); the target holds zeros there, recorded stale, so reads fail with EIO rather than return them -- and, the record covering the whole %u KiB column, so do reads of its other sectors without a checksum wherever they cannot be rebuilt either, once the replace finishes",
		     nr, stripe->logical, nr_data, BTRFS_STRIPE_LEN >> 10);
	return 0;
}

/*
 * Put a RAID5/6 data column on the replace target whole: every sector of it,
 * not only the ones an extent covers (see scrub_replace_copies_column()).
 *
 * Where each sector comes from:
 *
 * - A sector its checksum or its tree block's header verified is copied as the
 *   buffer holds it -- read from the source, or rebuilt and verified by the
 *   repair worker.  That is the only content anything has proved.
 *
 * - Data without a checksum is copied as the source returned it.  The source
 *   is a member in good standing here: every read of the sector returned
 *   exactly those bytes until now.  A rebuild of it, on the other hand, is
 *   checked by nothing -- the write-intent log vets it only against what the
 *   log records (mark_stale_sectors(), rbio_rebuilt_unverified()), so a
 *   parity that rotted, one torn by a crash while the log was off or before
 *   it was enabled, or a silently corrupt sibling goes straight onto the
 *   target.  Afterwards the stripe even agrees with itself (the target holds
 *   what the parity said), no scrub can find it, and the correct copy is gone
 *   with the source.  Copied, the data stays what reads returned, and any
 *   disagreement with the parity stays where a scrub finds it.  The data the
 *   source did not return -- a read fails as a whole, and the repair worker
 *   left an unverifiable rebuild in its place (scrub_note_unprovable()) -- is
 *   asked of the source again, one sector at a time, before anything else.
 *   Where the log names the source's column stale, or the source is missing,
 *   or the replace was told to avoid it ('-r'), the data is rebuilt instead,
 *   as below.
 *
 * - Free sectors, and what the source cannot give, are rebuilt from the other
 *   columns and the parity, leaving the source out (mirror 2).  The rebuild
 *   consults the write-intent log (mark_stale_sectors()) and refuses rather
 *   than guess when the log names more than the parity can cover
 *   (rbio_rebuilt_unverified()).  For a free sector it is also exactly the
 *   value needed: whatever the source held there, the target now holds what
 *   makes the parity true for its row, so a later rebuild of any OTHER column
 *   of that row comes out right -- even from a parity that rotted.
 *
 * - Where the log holds a record for this full stripe and does not name the
 *   source's column stale, the free sectors too are copied as the source has
 *   them.  The record is kept against the column's position, so from now on
 *   it describes the target, and carrying the column over unchanged leaves
 *   every question it records exactly as open as it was for whoever answers
 *   it (a scrub, the mount-time recovery): the source is what a read returned
 *   until now.  A rebuild would add nothing -- with another member named it
 *   is refused, or it gives back the source's own committed value.
 *
 * - A rebuild that is refused or cannot be done -- a sibling column or the
 *   parity unreadable, the log naming too much -- falls back to reading the
 *   source, if it is there, the log does not name its column stale, and it
 *   was not just asked.  That is what the extent-only copy wrote for such a
 *   sector of an extent, and what every read of it returned until now.
 *
 * - What is left cannot be had: a checksummed sector no mirror verified, or a
 *   sector neither a rebuild nor the source gives.  The target gets zeros
 *   there, recorded (scrub_replace_record_lost()) so that reading them fails
 *   rather than returns them.  Metadata never gets here:
 *   stripe_has_metadata_error() has aborted the replace already.
 *
 * Rebuilding first is right for a source nothing can vouch for; a device that
 * has failed for writes and missed some of them will be one.  One that has
 * not is the best copy there is of what it holds.
 *
 * Returns 0, or -EIO to abort the replace.
 */
static int scrub_replace_copy_column(struct scrub_ctx *sctx,
				     struct scrub_stripe *stripe)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	const unsigned int nr = stripe_length(stripe) >> fs_info->sectorsize_bits;
	const unsigned long has_extent = scrub_bitmap_read_has_extent(stripe);
	const unsigned long is_metadata = scrub_bitmap_read_is_metadata(stripe);
	const unsigned long error = scrub_bitmap_read_error(stripe);
	/* The buffer holds the source's bytes, not a rebuild (-r, or no source). */
	const bool read_source = stripe->dev->bdev &&
		fs_info->dev_replace.cont_reading_from_srcdev_mode !=
		BTRFS_DEV_REPLACE_ITEM_CONT_READING_FROM_SRCDEV_MODE_AVOID;
	const bool trust_source = stripe->dev->bdev &&
				  !btrfs_wib_stale(fs_info, stripe->logical);
	const bool rebuild_first = READ_ONCE(replace_rebuilds_unchecked);
	unsigned long column = 0;
	unsigned long verified = 0;
	unsigned long lost = 0;
	/* Data without a checksum to ask the source for again, before rebuilding. */
	unsigned long ask_source = 0;
	unsigned long rest;
	int sector_nr;
	int ret;

	ASSERT(stripe->raid56_nr_data);
	bitmap_set(&column, 0, nr);
	for_each_set_bit(sector_nr, &has_extent, nr) {
		if (!test_bit(sector_nr, &is_metadata) &&
		    !stripe->sectors[sector_nr].csum)
			continue;
		/* Every mirror was tried by the repair worker. */
		if (test_bit(sector_nr, &error))
			__set_bit(sector_nr, &lost);
		else
			__set_bit(sector_nr, &verified);
	}
	bitmap_andnot(&rest, &column, &verified, nr);
	bitmap_andnot(&rest, &rest, &lost, nr);

	if (read_source && trust_source) {
		if (btrfs_wib_stripe_error(fs_info, stripe->raid56_full_stripe,
					   stripe->raid56_nr_data) !=
		    BTRFS_WIB_STRIPE_NO_ERROR) {
			/*
			 * A recorded stripe: the source's bytes already in the
			 * buffer go over as they are.  Only what it did not
			 * return is left to find.
			 */
			bitmap_and(&rest, &rest, &error, nr);
		} else if (!rebuild_first) {
			/*
			 * Data the source returned goes over as it is; free
			 * sectors are rebuilt.
			 */
			unsigned long returned;

			bitmap_andnot(&returned, &has_extent, &error, nr);
			bitmap_andnot(&rest, &rest, &returned, nr);
		}
		if (!rebuild_first)
			bitmap_and(&ask_source, &rest, &has_extent, nr);
	}

	if (!bitmap_empty(&ask_source, nr)) {
		const unsigned long failed = scrub_replace_read(stripe, ask_source,
								stripe->mirror_num);

		bitmap_andnot(&rest, &rest, &ask_source, nr);
		bitmap_or(&rest, &rest, &failed, nr);
	}

	if (!bitmap_empty(&rest, nr)) {
		unsigned long failed = scrub_replace_read(stripe, rest, 2);
		unsigned long again;

		/* The source has just failed to give the ones it was asked for. */
		bitmap_andnot(&again, &failed, &ask_source, nr);
		if (!bitmap_empty(&again, nr) && trust_source) {
			bitmap_andnot(&failed, &failed, &again, nr);
			again = scrub_replace_read(stripe, again, stripe->mirror_num);
			bitmap_or(&failed, &failed, &again, nr);
		}
		bitmap_or(&lost, &lost, &failed, nr);
	}

	if (!bitmap_empty(&lost, nr)) {
		ret = scrub_replace_record_lost(sctx, stripe, lost);
		if (ret < 0)
			return ret;
		for_each_set_bit(sector_nr, &lost, nr)
			memset(stripe->buffer + (sector_nr << fs_info->sectorsize_bits),
			       0, fs_info->sectorsize);
	}
	scrub_write_sectors(sctx, stripe, column, true);
	return 0;
}

static int flush_scrub_stripes(struct scrub_ctx *sctx)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct scrub_stripe *stripe;
	const int nr_stripes = sctx->cur_stripe;
	int ret = 0;

	if (!nr_stripes)
		return 0;

	ASSERT(test_bit(SCRUB_STRIPE_FLAG_INITIALIZED, &sctx->stripes[0].state));

	/* Submit the stripes which are populated but not submitted. */
	if (nr_stripes % SCRUB_STRIPES_PER_GROUP) {
		const int first_slot = round_down(nr_stripes, SCRUB_STRIPES_PER_GROUP);

		submit_initial_group_read(sctx, first_slot, nr_stripes - first_slot);
	}

	for (int i = 0; i < nr_stripes; i++) {
		stripe = &sctx->stripes[i];

		wait_event(stripe->repair_wait,
			   test_bit(SCRUB_STRIPE_FLAG_REPAIR_DONE, &stripe->state));
	}

	/* Submit for dev-replace. */
	if (sctx->is_dev_replace) {
		/*
		 * For dev-replace, if we know there is something wrong with
		 * metadata, we should immediately abort.
		 */
		for (int i = 0; i < nr_stripes; i++) {
			if (unlikely(stripe_has_metadata_error(&sctx->stripes[i]))) {
				ret = -EIO;
				goto out;
			}
		}
		for (int i = 0; i < nr_stripes; i++) {
			unsigned long good;
			unsigned long has_extent;
			unsigned long error;

			stripe = &sctx->stripes[i];

			ASSERT(stripe->dev == fs_info->dev_replace.srcdev);

			/*
			 * The repair write-backs' errors were consumed by the
			 * report (REPAIR_DONE); from here the bitmap is the
			 * target's, read below.
			 */
			stripe->write_error_bitmap = 0;
			/*
			 * A column that could not be recorded aborts the
			 * replace: copy nothing more, but still wait below for
			 * what was already submitted.
			 */
			if (test_bit(SCRUB_STRIPE_FLAG_WHOLE_COLUMN, &stripe->state)) {
				if (!ret)
					ret = scrub_replace_copy_column(sctx, stripe);
				continue;
			}
			has_extent = scrub_bitmap_read_has_extent(stripe);
			error = scrub_bitmap_read_error(stripe);
			bitmap_andnot(&good, &has_extent, &error, stripe->nr_sectors);
			scrub_write_sectors(sctx, stripe, good, true);
		}
	}

	/* Wait for the above writebacks to finish. */
	for (int i = 0; i < nr_stripes; i++) {
		stripe = &sctx->stripes[i];

		wait_scrub_stripe_io(stripe);
		/*
		 * A sector the target did not take is missing from it, and the
		 * replace must not finish as if it were there: on a profile
		 * with one copy that loses the only good one.  Count it, and
		 * scrub_enumerate_chunks() aborts the replace with EIO.  The
		 * count was lost when this write path replaced the old one.
		 */
		if (sctx->is_dev_replace && stripe->write_error_bitmap &&
		    !btrfs_scrub_replace_ignores_write_errors()) {
			const int nr = bitmap_weight(&stripe->write_error_bitmap,
						     stripe->nr_sectors);

			atomic64_add(nr, &fs_info->dev_replace.num_write_errors);
			btrfs_err_rl(fs_info,
"scrub: device replace could not write %d sector(s) at logical %llu to the target device; the replace will be aborted",
				     nr, stripe->logical);
		}
		spin_lock(&sctx->stat_lock);
		sctx->stat.last_physical = stripe->physical + stripe_length(stripe);
		spin_unlock(&sctx->stat_lock);
		scrub_reset_stripe(stripe);
	}
out:
	sctx->cur_stripe = 0;
	return ret;
}

static void raid56_scrub_wait_endio(struct bio *bio)
{
	complete(bio->bi_private);
}

/*
 * The slot at sctx->cur_stripe has just been filled: count it, submit its group
 * once that is complete, and flush them all once every slot is in use.
 */
static int scrub_stripe_queued(struct scrub_ctx *sctx)
{
	sctx->cur_stripe++;

	/* We filled one group, submit it. */
	if (sctx->cur_stripe % SCRUB_STRIPES_PER_GROUP == 0) {
		const int first_slot = sctx->cur_stripe - SCRUB_STRIPES_PER_GROUP;

		submit_initial_group_read(sctx, first_slot, SCRUB_STRIPES_PER_GROUP);
	}

	/* Last slot used, flush them all. */
	if (sctx->cur_stripe == SCRUB_TOTAL_STRIPES)
		return flush_scrub_stripes(sctx);
	return 0;
}

static int queue_scrub_stripe(struct scrub_ctx *sctx, struct btrfs_block_group *bg,
			      struct btrfs_device *dev, int mirror_num,
			      u64 logical, u32 length, u64 physical,
			      u64 *found_logical_ret)
{
	struct scrub_stripe *stripe;
	int ret;

	/*
	 * There should always be one slot left, as caller filling the last
	 * slot should flush them all.
	 */
	ASSERT(sctx->cur_stripe < SCRUB_TOTAL_STRIPES);

	/* @found_logical_ret must be specified. */
	ASSERT(found_logical_ret);

	stripe = &sctx->stripes[sctx->cur_stripe];
	scrub_reset_stripe(stripe);
	ret = scrub_find_fill_first_stripe(bg, &sctx->extent_path,
					   &sctx->csum_path, dev, physical,
					   mirror_num, logical, length, stripe);
	/* Either >0 as no more extents or <0 for error. */
	if (ret)
		return ret;
	*found_logical_ret = stripe->logical;
	return scrub_stripe_queued(sctx);
}

/*
 * Return 0 if we should not cancel the scrub.
 * Return <0 if we need to cancel the scrub, returned value will
 * indicate the reason:
 * - -ECANCELED - Being explicitly canceled through ioctl.
 * - -EINTR     - Being interrupted by signal or fs/process freezing.
 */
static int should_cancel_scrub(const struct scrub_ctx *sctx)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;

	/*
	 * A recovery at mount holds s_umount and must not wait for a
	 * transaction, so it does not take part in the scrub pause protocol
	 * below.  It does have to stay killable: it can run for a long time
	 * on a large log, and a mount that ignores a fatal signal is
	 * unkillable from userspace.  Aborting that way is safe -- the on-disk
	 * log is only rewritten after the whole pass, so the next mount redoes
	 * it.
	 *
	 * It deliberately does not answer scrub_cancel_req.  The user did not
	 * start this scrub, and cancelling it is not safe the way a signal is:
	 * the recovery loop treats a per-stripe failure as "keep it recorded"
	 * and carries on, so a cancel makes every remaining stripe fail, then
	 * the pass still declares itself done and rewrites the log.  Records
	 * that no longer fit are dropped with a warning by
	 * btrfs_wib_add_sticky(), and the filesystem continues read-write with
	 * stripes whose parity was never regenerated.
	 */
	if (sctx->internal) {
		if (fatal_signal_pending(current))
			return -EINTR;
		if (btrfs_fs_closing(fs_info))
			return -EINTR;
		return 0;
	}

	if (atomic_read(&fs_info->scrub_cancel_req) ||
	    atomic_read(&sctx->cancel_req))
		return -ECANCELED;

	/*
	 * The write-intent log dropped a record of zeros this replace put on
	 * its target: the replace cannot finish (btrfs_wib_replace_end()), so
	 * stop copying now rather than at the end.
	 */
	if (sctx->is_dev_replace && btrfs_wib_replace_marks_lost(fs_info))
		return -EIO;

	/*
	 * The user (e.g. fsfreeze command) or power management (PM)
	 * suspend/hibernate can freeze the fs.  And PM suspend/hibernate will
	 * also freeze all user processes.
	 *
	 * A user process can only be frozen when it is in user space, thus we
	 * have to cancel the run so that the process can return to the user
	 * space.
	 *
	 * Furthermore we have to check both filesystem and process freezing,
	 * as PM can be configured to freeze the filesystems before processes.
	 *
	 * If we only check fs freezing, then suspend without fs freezing
	 * will timeout, as the process is still in kernel space.
	 *
	 * If we only check process freezing, then suspend with fs freezing
	 * will timeout, as the running scrub will prevent the fs from being frozen.
	 */
	if (fs_info->sb->s_writers.frozen > SB_UNFROZEN ||
	    freezing(current) || signal_pending(current))
		return -EINTR;
	return 0;
}

/*
 * Does anything in [@start, @start + @len) of @bg hold an extent, in the commit
 * root every other lookup of the copy uses?  Returns 1 if so, 0 if not.
 */
static int scrub_range_has_extent(struct btrfs_block_group *bg, u64 start, u64 len)
{
	BTRFS_PATH_AUTO_RELEASE(path);
	struct btrfs_fs_info *fs_info = bg->fs_info;
	struct btrfs_root *extent_root = btrfs_extent_root(fs_info, bg->start);
	int ret;

	if (unlikely(!extent_root)) {
		btrfs_err(fs_info, "scrub: no valid extent root found");
		return -EUCLEAN;
	}
	path.search_commit_root = true;
	path.skip_locking = true;
	ret = find_first_extent_item(extent_root, &path, start, len);
	if (ret < 0)
		return ret;
	return ret == 0;
}

/*
 * RAID5/6 device replace copying whole columns: queue the source's data column
 * @logical of the full stripe at @full_stripe_start, if anything at all in that
 * full stripe is allocated.
 *
 * scrub_simple_mirror() only finds a column that holds an extent itself.  A
 * column with none, in a full stripe whose other columns hold data, is as much
 * a term of that stripe's parity as a used one (scrub_replace_copies_column()),
 * so it is queued too, as a stripe without extents, and
 * scrub_replace_copy_column() rebuilds all of it.  A full stripe with no
 * extent anywhere is left alone: there is nothing in it to rebuild, and a
 * write into it computes the parity of the rows it touches afresh from
 * whatever the columns hold.
 */
static int queue_raid56_replace_column(struct scrub_ctx *sctx,
				       struct btrfs_block_group *bg,
				       struct btrfs_device *dev, u64 logical,
				       u64 physical, u64 full_stripe_start,
				       int nr_data)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct scrub_stripe *stripe;
	int ret;

	ret = should_cancel_scrub(sctx);
	if (ret < 0)
		return ret;
	if (atomic_read(&fs_info->scrub_pause_req))
		scrub_blocked_if_needed(fs_info);
	spin_lock(&bg->lock);
	if (test_bit(BLOCK_GROUP_FLAG_REMOVED, &bg->runtime_flags)) {
		spin_unlock(&bg->lock);
		return 0;
	}
	spin_unlock(&bg->lock);

	/* See queue_scrub_stripe(). */
	ASSERT(sctx->cur_stripe < SCRUB_TOTAL_STRIPES);
	stripe = &sctx->stripes[sctx->cur_stripe];
	scrub_reset_stripe(stripe);
	ret = scrub_find_fill_first_stripe(bg, &sctx->extent_path,
					   &sctx->csum_path, dev, physical, 1,
					   logical, BTRFS_STRIPE_LEN, stripe);
	if (ret < 0)
		return ret;
	if (ret > 0) {
		ret = scrub_range_has_extent(bg, full_stripe_start,
					     btrfs_stripe_nr_to_offset(nr_data));
		if (ret <= 0)
			return ret;
		/* As scrub_raid56_parity_stripe() does for an empty column. */
		stripe->logical = logical;
		stripe->physical = physical;
		stripe->dev = dev;
		stripe->bg = bg;
		stripe->mirror_num = 1;
		set_bit(SCRUB_STRIPE_FLAG_INITIALIZED, &stripe->state);
	}
	ASSERT(stripe->logical == logical);
	set_bit(SCRUB_STRIPE_FLAG_WHOLE_COLUMN, &stripe->state);
	stripe->raid56_full_stripe = full_stripe_start;
	stripe->raid56_nr_data = nr_data;
	return scrub_stripe_queued(sctx);
}

static int scrub_raid56_cached_parity(struct scrub_ctx *sctx,
				      struct btrfs_device *scrub_dev,
				      struct btrfs_chunk_map *map,
				      u64 full_stripe_start,
				      unsigned long *extent_bitmap)
{
	DECLARE_COMPLETION_ONSTACK(io_done);
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct btrfs_io_context *bioc = NULL;
	struct btrfs_raid_bio *rbio;
	struct bio bio;
	const int data_stripes = nr_data_stripes(map);
	u64 length = btrfs_stripe_nr_to_offset(data_stripes);
	int ret;

	bio_init(&bio, NULL, NULL, 0, REQ_OP_READ);
	bio.bi_iter.bi_sector = full_stripe_start >> SECTOR_SHIFT;
	bio.bi_private = &io_done;
	bio.bi_end_io = raid56_scrub_wait_endio;

	btrfs_bio_counter_inc_blocked(fs_info);
	ret = btrfs_map_block(fs_info, BTRFS_MAP_WRITE, full_stripe_start,
			      &length, &bioc, NULL, NULL);
	if (ret < 0)
		goto out;
	/* For RAID56 write there must be an @bioc allocated. */
	ASSERT(bioc);
	rbio = raid56_parity_alloc_scrub_rbio(&bio, bioc, scrub_dev, extent_bitmap,
				BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits);
	btrfs_put_bioc(bioc);
	if (!rbio) {
		ret = -ENOMEM;
		goto out;
	}
	if (sctx->internal)
		raid56_parity_scrub_rbio_strict(rbio);
	/* Use the recovered stripes as cache to avoid read them from disk again. */
	for (int i = 0; i < data_stripes; i++) {
		struct scrub_stripe *stripe = &sctx->raid56_data_stripes[i];

		raid56_parity_cache_data_folios(rbio, stripe->buffer,
				full_stripe_start + (i << BTRFS_STRIPE_LEN_SHIFT));
	}
	raid56_parity_submit_scrub_rbio(rbio);
	wait_for_completion_io(&io_done);
	ret = blk_status_to_errno(bio.bi_status);
out:
	btrfs_bio_counter_dec(fs_info);
	bio_uninit(&bio);
	return ret;
}

/*
 * @regen_parity: after the data stripes have been verified and repaired,
 * check and rewrite the parity on @scrub_dev.  The write-intent log
 * recovery skips that while extents may still be hidden in the tree log:
 * the parity is recomputed from every sector of a vertical stripe that
 * holds an extent, including sectors this scrub could not verify.
 */
/*
 * What the write-intent log lets this scrub do about one full stripe.
 *
 * Repair what can be proved, preserve what cannot, never guess.  The record
 * distinguishes the two by construction: @stale is set only where the rbio
 * supplied that data column and that column's own write failed, so it names a
 * member.  @sticky without @stale says a write went wrong somewhere in the
 * stripe without saying where, and there is nothing to act on in that.
 */
enum scrub_wib_plan {
	/* Nothing recorded, or nothing recorded that needs our help. */
	SCRUB_WIB_NONE,
	/* Named members, and enough good parity to rebuild them. */
	SCRUB_WIB_PROVEN,
	/* Named members, but not enough left to rebuild them without guessing. */
	SCRUB_WIB_AMBIGUOUS,
	/*
	 * Named members and just enough good parity to rebuild them, but a
	 * write into the stripe may have been torn and none is left over to
	 * check the rebuild with.
	 */
	SCRUB_WIB_TORN,
};

/*
 * Does this data column hold anything only the log can vouch for?
 *
 * A sector with a data checksum, or a metadata sector with its tree-block
 * header and generation, carries its own proof and the ordinary scrub path
 * verifies and repairs it.  Blocking that would be the opposite of helping.
 */
static bool scrub_stripe_has_unverifiable(struct scrub_stripe *stripe)
{
	const unsigned long has_extent = scrub_bitmap_read_has_extent(stripe);
	const unsigned long is_metadata = scrub_bitmap_read_is_metadata(stripe);
	int sector_nr;

	for_each_set_bit(sector_nr, &has_extent, stripe->nr_sectors) {
		if (test_bit(sector_nr, &is_metadata))
			continue;
		if (!stripe->sectors[sector_nr].csum)
			return true;
	}
	return false;
}

/*
 * Copy the data columns of a full stripe the scrub is about to walk away from.
 *
 * This is the only moment they are all in memory at once, mutually coherent
 * (the block group is read-only for the whole chunk scrub, so nothing is
 * writing) and not yet overwritten.  A helper that armed the evidence channel
 * gets them; if nobody armed it this costs one unlocked pointer read.
 *
 * The parity is named, not copied.  Every column's devid and physical offset
 * is handed over, and the block group stays read-only until this chunk's scrub
 * ends, so a helper reads the parity off the device without racing anything --
 * rather than this path issuing more I/O to an array that is failing by
 * construction, on a path with no REQ_FAILFAST and no timeout.
 *
 * The column-to-device mapping is derived rather than looked up, so it is
 * checked against the columns scrub already resolved: if the derivation cannot
 * reproduce those, the parity entries it produces are not trustworthy either
 * and the capture is abandoned rather than handed over wrong.
 */
static void scrub_capture_evidence(struct scrub_ctx *sctx,
				   struct btrfs_chunk_map *map,
				   struct btrfs_block_group *bg,
				   u64 full_stripe_start, int data_stripes)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	const int nr_parity = map->num_stripes - data_stripes;
	const u64 fstripe_len = btrfs_stripe_nr_to_offset(data_stripes);
	struct btrfs_raid56_evidence_slot *slot;
	struct btrfs_wib_stripe_state st = { 0 };
	u64 rot;
	u32 rem;

	if (!btrfs_raid56_evidence_armed(fs_info))
		return;

	rot = div_u64_rem(full_stripe_start - bg->start, fstripe_len, &rem);
	if (rem)
		return;

	slot = btrfs_raid56_evidence_claim(fs_info, data_stripes, nr_parity);
	if (!slot)
		return;

	for (int c = 0; c < map->num_stripes; c++) {
		const int idx = (rot + c) % map->num_stripes;
		const struct btrfs_device *dev = map->stripes[idx].dev;

		slot->devid[c] = dev ? dev->devid : 0;
		slot->physical[c] = map->stripes[idx].physical +
				    (rot << BTRFS_STRIPE_LEN_SHIFT);
	}
	/* The derivation has to reproduce what scrub already resolved. */
	for (int i = 0; i < data_stripes; i++) {
		const struct scrub_stripe *stripe = &sctx->raid56_data_stripes[i];

		if (!stripe->dev || slot->devid[i] != stripe->dev->devid ||
		    slot->physical[i] != stripe->physical) {
			btrfs_warn_rl(fs_info,
"scrub: not capturing evidence for full stripe %llu: the column mapping does not agree with the stripes already read",
				      full_stripe_start);
			slot->nr_bytes = 0;
			slot->full_stripe_start = 0;
			btrfs_raid56_evidence_commit(fs_info);
			return;
		}
	}

	slot->full_stripe_start = full_stripe_start;
	/*
	 * Only the user-scrub path holds the block group read-only
	 * (btrfs_inc_block_group_ro(), and mandatory for RAID56 there).  The
	 * write-intent log's mount-time recovery reaches this same verdict
	 * with no such hold, so its columns can have been read either side of
	 * a write.  Capture it anyway -- it is the earliest and only moment
	 * those bytes exist together, and the crash nobody was awake for is
	 * exactly the case this is for -- but say which it was.
	 */
	if (bg->ro)
		slot->record_flags |= BTRFS_RAID56_EVIDENCE_F_COHERENT;
	if (btrfs_wib_stripe_state(fs_info, full_stripe_start, data_stripes,
				   nr_parity, &st)) {
		slot->stale_cols = st.stale_cols;
		slot->bad_parity = st.bad_parity;
		slot->gen = st.gen;
	}
	for (int i = 0; i < data_stripes; i++)
		memcpy((u8 *)slot->data + ((size_t)i << BTRFS_STRIPE_LEN_SHIFT),
		       sctx->raid56_data_stripes[i].buffer, BTRFS_STRIPE_LEN);
	slot->nr_bytes = (u32)data_stripes << BTRFS_STRIPE_LEN_SHIFT;
	btrfs_raid56_evidence_commit(fs_info);
}

/*
 * Testing only: look up the devices of a full stripe's parities where they sit
 * in full stripe 0, without the rotation, as scrub_raid56_plan_wib() did
 * before.  The negative control for uml/parity_rotation.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool plan_parity_unrotated;
module_param_named(raid56_scrub_parity_unrotated, plan_parity_unrotated, bool, 0644);
MODULE_PARM_DESC(raid56_scrub_parity_unrotated,
		 "Count a full stripe's usable parities on the devices that hold them in full stripe 0, ignoring the RAID5/6 rotation (testing only: restores a known defect)");
#else
static const bool plan_parity_unrotated;
#endif

/*
 * Testing only: rebuild the columns the write-intent log names from as many
 * parities as there are such columns although a write into their full stripe
 * may have been torn, with none left over to check the rebuild, and write it
 * back -- as scrub_raid56_plan_wib() did before SCRUB_WIB_TORN.  The negative
 * control for the named arms of uml/torn_present.sh.
 *
 * And let the verify pass of scrub_raid56_recover_absent() act on the record
 * like any other pass, as it did before @raid56_verify_only: it rebuilt such a
 * column and wrote it back before the classification had decided anything.
 * Needs raid56_scrub_torn_trusts_parity=1 as well to show, since the verify
 * pass only ever meets a possibly torn stripe; the negative control for the
 * classify arms of uml/torn_present.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool torn_trusts_parity;
module_param_named(raid56_scrub_torn_trusts_parity, torn_trusts_parity, bool, 0644);
MODULE_PARM_DESC(raid56_scrub_torn_trusts_parity,
		 "Rebuild a data column the write-intent log names stale from the last parity left although a write into its full stripe may have been torn, and write it back (testing only: restores a known defect)");
static bool verify_pass_rebuilds;
module_param_named(raid56_recover_absent_verify_rebuilds, verify_pass_rebuilds, bool, 0644);
MODULE_PARM_DESC(raid56_recover_absent_verify_rebuilds,
		 "Let the recovery's verify pass over a full stripe with a data column on a missing device rebuild and write back the columns the write-intent log names stale, before it has classified the stripe (testing only: restores a known defect)");
#else
static const bool torn_trusts_parity;
static const bool verify_pass_rebuilds;
#endif

static enum scrub_wib_plan scrub_raid56_plan_wib(struct scrub_ctx *sctx,
						 struct btrfs_chunk_map *map,
						 u64 full_stripe_start,
						 int data_stripes,
						 u64 *holes_out)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	const int nr_parity = map->num_stripes - data_stripes;
	struct btrfs_wib_stripe_state st;
	u64 holes = 0, needs_help = 0;
	int nr_good_par = 0;
	int rot;

	*holes_out = 0;
	if (likely(!btrfs_wib_any_stale(fs_info)))
		return SCRUB_WIB_NONE;
	/*
	 * The verify pass of a classification: whatever the record names is
	 * what the classification weighs, and a rebuild written back now would
	 * have been written before it decided -- from both parities, which a
	 * torn write may have left not describing the data, over a column the
	 * crash may have left right.  Nothing is written for it here, and the
	 * classification writes only what it decides.
	 */
	if (sctx->raid56_verify_only && !READ_ONCE(verify_pass_rebuilds))
		return SCRUB_WIB_NONE;
	/*
	 * Wider than the masks below can express, so the log cannot be
	 * consulted for this stripe.  That is not the same as the log having
	 * nothing to say: we only get here when some stripe somewhere IS
	 * recorded stale, and returning SCRUB_WIB_NONE would send this stripe
	 * down the ordinary path, where regenerating the parity from data that
	 * may be stale is exactly the destruction this feature exists to
	 * prevent.  Decline instead -- the same answer given to a stripe whose
	 * ambiguity is understood, for the same reason.
	 */
	if (data_stripes > 64 || nr_parity > 2) {
		btrfs_warn_rl(fs_info,
"scrub: full stripe %llu has %d data stripes, more than the write-intent log can describe; declining to regenerate its parity while any stripe on this filesystem is recorded stale",
			      full_stripe_start, data_stripes);
		return SCRUB_WIB_AMBIGUOUS;
	}
	if (!btrfs_wib_stripe_state(fs_info, full_stripe_start, data_stripes,
				    nr_parity, &st))
		return SCRUB_WIB_NONE;
	/*
	 * Some of the record, not all of it.  Records cover whole full
	 * stripes, so the rest was dropped when the log filled up, and with it
	 * whatever it named: a column missing from @st.stale_cols may be one
	 * whose bit went with the dropped half.  For content with a checksum
	 * the ordinary path finds out for itself; for the rest nothing can.
	 */
	if (btrfs_wib_stripe_error(fs_info, full_stripe_start, data_stripes) ==
	    BTRFS_WIB_STRIPE_PARTIAL_ERROR) {
		for (int i = 0; i < data_stripes; i++) {
			struct scrub_stripe *stripe = &sctx->raid56_data_stripes[i];

			if (!stripe->dev || !stripe->dev->bdev ||
			    !scrub_stripe_has_unverifiable(stripe))
				continue;
			btrfs_warn_rl(fs_info,
"scrub: full stripe %llu straddles a log region whose record was dropped, and some of it has no checksum; keeping the record rather than guessing",
				      full_stripe_start);
			return SCRUB_WIB_AMBIGUOUS;
		}
	}

	/*
	 * Two different questions, and conflating them is a hole.
	 *
	 * @holes is every column that cannot be BELIEVED: named stale, or on a
	 * device that is not there.  It has nothing to do with checksums --
	 * a reconstruction is determined by columns and parities, not by which
	 * sectors happen to carry their own proof.  This has to match what
	 * mark_stale_sectors() will count when the repair actually runs, or
	 * this function authorises a rebuild that the recovery path then
	 * declines to honour -- and declining there is silent: it returns
	 * without marking, so the column the log names is read off the disk
	 * and believed, and on this path the result is written back.
	 *
	 * @needs_help is the narrower question of whether any of that is our
	 * business: a column whose content is checksummed, or is a metadata
	 * tree block, is verified and repaired by the ordinary path and must
	 * not be blocked.
	 */
	for (int i = 0; i < data_stripes; i++) {
		struct scrub_stripe *stripe = &sctx->raid56_data_stripes[i];

		if (!stripe->dev || !stripe->dev->bdev) {
			holes |= BIT_ULL(i);
			continue;
		}
		if (!(st.stale_cols & BIT_ULL(i)))
			continue;
		holes |= BIT_ULL(i);
		if (scrub_stripe_has_unverifiable(stripe))
			needs_help |= BIT_ULL(i);
	}
	if (!needs_help)
		return SCRUB_WIB_NONE;

	/*
	 * Parities that can still be used as a source.
	 *
	 * Parity p is column data_stripes + p of the full stripe, and the
	 * columns rotate by one device per full stripe: column c of full
	 * stripe n sits on map->stripes[(c + n) % num_stripes].  That is how
	 * btrfs_map_block() lays out the bioc the rebuild will run on, and how
	 * scrub_raid56_parity_stripe() resolved the data columns examined
	 * above.  map->stripes[data_stripes + p] is where parity p sits in
	 * full stripe 0 only; everywhere else it is another column's device,
	 * so this used to ask about the wrong device on all but one full
	 * stripe in num_stripes.
	 *
	 * Only a device that is not there makes the answer differ, and then
	 * it is wrong both ways.  A missing device holding a data column is
	 * taken for a missing parity: the stripe is declared ambiguous and
	 * left without its redundancy, although the parities that really are
	 * there -- the ones the rebuild would use -- suffice.  And a missing
	 * parity is taken for a present one: the budget below passes a stripe
	 * with more unknowns than equations, and all that stands between it
	 * and a rebuild that invents a value is the recovery's own count in
	 * mark_stale_sectors().
	 *
	 * @st.bad_parity is indexed by parity, not by device: no rotation.
	 */
	rot = div_u64(full_stripe_start - map->start,
		      data_stripes) >> BTRFS_STRIPE_LEN_SHIFT;
	for (int p = 0; p < nr_parity; p++) {
		int idx = data_stripes + p;
		const struct btrfs_device *dev;

		if (!READ_ONCE(plan_parity_unrotated))
			idx = (idx + rot) % map->num_stripes;
		dev = map->stripes[idx].dev;
		if (dev && dev->bdev && !(st.bad_parity & BIT(p)))
			nr_good_par++;
	}

	*holes_out = holes;
	/*
	 * More unknowns than equations: the reconstruction is not determined,
	 * and without a checksum a rebuild that runs out of equations does not
	 * fail loudly, it returns a value nothing ever committed.  Leave every
	 * byte of the stripe alone, keep the record, and let a recovery tool
	 * that can involve a human decide.
	 */
	if (hweight64(holes) > nr_good_par)
		return SCRUB_WIB_AMBIGUOUS;
	/*
	 * Enough equations, but a write into the stripe may have been torn:
	 * in flight at a crash (@raid56_torn, the recovery's), or marked
	 * possibly torn in the live table.  Its parity then may not describe
	 * the data in the rows that write touched, and a rebuild that spends
	 * every good parity on the named columns returns there the named
	 * column xor whatever the torn write changed -- over a sector the
	 * crash may have left right, and a checksum is what these columns
	 * do not have.  With a parity left over the read path checks each
	 * rebuilt row against it (recover_verify_q()) and fails the ones it
	 * contradicts; without one, nothing can.  Leave the stripe as it is;
	 * the recovery records it undecidable (btrfs_scrub_raid56_full_stripe()).
	 */
	if (hweight64(holes) == nr_good_par && !READ_ONCE(torn_trusts_parity) &&
	    (sctx->raid56_torn ||
	     btrfs_wib_stripe_torn(fs_info, full_stripe_start,
				   btrfs_stripe_nr_to_offset(data_stripes))))
		return SCRUB_WIB_TORN;

	for (int i = 0; i < data_stripes; i++)
		if (needs_help & BIT_ULL(i))
			sctx->raid56_data_stripes[i].wib_rebuild = true;
	return SCRUB_WIB_PROVEN;
}

/*
 * Testing only: let a RAID5/6 device replace leave the target's parity of a
 * full stripe it declines to regenerate as whatever the new device held, as it
 * did before scrub_replace_copy_parity(), while the data columns are still
 * copied whole.  raid56_wf_replace_extents_only restores this too, but along
 * with the rest of the extent-only copy, whose own holes would stand beside
 * this one in the same test.  The negative control for the replace arms of
 * uml/forced_stale.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool replace_skips_refused_parity;
module_param_named(raid56_wf_replace_skips_refused_parity,
		   replace_skips_refused_parity, bool, 0644);
MODULE_PARM_DESC(raid56_wf_replace_skips_refused_parity,
		 "Let a RAID5/6 device replace leave the target's parity of a full stripe scrub declines to regenerate as whatever the new device held, recording nothing (testing only: restores a known defect)");
#else
static const bool replace_skips_refused_parity;
#endif

/*
 * A replace is leaving the full stripe at @full_stripe_start without
 * regenerating its parity -- the write-intent log cannot decide the stripe, or
 * a data column holds sectors nothing could repair -- and so without writing
 * @src's parity column of it to the target at all.  The target would keep
 * whatever it held there, and nothing would say so.  For a stripe the log
 * declines to decide that parity is the only copy of what was acknowledged:
 * lost, and worse, once a missing device comes back and the stripe becomes
 * decidable, a rebuild the log calls proven would be computed out of the
 * target's junk.
 *
 * Nor does it take a missing device.  Say the log names a data column stale
 * and @src, which holds the parity, is there but does not return it: the
 * rebuild of the column cannot run, the scrub keeps the column in error rather
 * than take its stale bytes back (scrub_verify_repair_read()), and the stripe
 * is left unrepaired with its record as it was -- that column named, no parity
 * bad.  After the replace that record describes the target, and the target
 * counts as good parity.  Every read of the column, its unchanged sectors along
 * with the stale one (the record does not say which), is then rebuilt out of
 * the target's junk and returned without an error: there is no checksum, and
 * a rebuild the read path does not persist it still returns.  The next scrub
 * -- what the alerts tell the administrator to run -- finds the stripe proven,
 * rebuilds the column the same way and writes it over data that was right.
 *
 * So copy it as it is.  The source's parity is what the array has had all
 * along, so the stripe's state carries over exactly, its record included.
 * What cannot be copied -- the source is missing, or does not return it --
 * gets zeros, and the log records this parity stale so that no rebuild uses
 * it; the sectors are counted and the alert raised as for a data column
 * (scrub_replace_record_lost()).  If that cannot be recorded either, the
 * replace is aborted.  In the case above, recording the parity stale makes the
 * stripe undecidable, which is the truth -- the acknowledged value was in the
 * parity that did not read -- so reads of the named column fail with EIO and
 * scrubs leave it alone (SCRUB_WIB_AMBIGUOUS) instead of inventing it.
 *
 * Plain block I/O, a page at a time: this is the exception, and the source's
 * parity column has no logical address to go through the mapping with.
 *
 * Returns 0, or a negative error to abort the replace.
 */
static int scrub_replace_copy_parity(struct scrub_ctx *sctx,
				     struct btrfs_device *src,
				     struct btrfs_block_group *bg,
				     struct btrfs_chunk_map *map,
				     u64 full_stripe_start)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct btrfs_device *tgt = sctx->wr_tgtdev;
	const int data_stripes = nr_data_stripes(map);
	const u32 rot = div_u64(full_stripe_start - bg->start, data_stripes) >>
			BTRFS_STRIPE_LEN_SHIFT;
	const char *why;
	struct page *page;
	void *buf;
	u64 physical = 0;
	u32 lost = 0;
	u32 nr_lost;
	int parity = -1;
	int ret = 0;

	if (READ_ONCE(replace_skips_refused_parity))
		return 0;

	/* Which parity of this full stripe the source holds, and where. */
	for (int c = data_stripes; c < map->num_stripes; c++) {
		const int idx = (c + rot) % map->num_stripes;

		if (map->stripes[idx].dev == src) {
			parity = c - data_stripes;
			physical = map->stripes[idx].physical +
				   btrfs_stripe_nr_to_offset(rot);
			break;
		}
	}
	if (unlikely(parity < 0 || !tgt || !tgt->bdev)) {
		btrfs_err(fs_info,
"scrub: device replace found no parity of devid %llu in full stripe %llu to copy",
			  src->devid, full_stripe_start);
		return -EUCLEAN;
	}

	page = alloc_page(GFP_NOFS);
	if (!page)
		return -ENOMEM;
	buf = page_address(page);
	for (u32 off = 0; off < BTRFS_STRIPE_LEN; off += PAGE_SIZE) {
		const sector_t sector = (physical + off) >> SECTOR_SHIFT;

		if (!src->bdev ||
		    bdev_rw_virt(src->bdev, sector, buf, PAGE_SIZE, REQ_OP_READ)) {
			memset(buf, 0, PAGE_SIZE);
			lost += PAGE_SIZE;
		}
		if (bdev_rw_virt(tgt->bdev, sector, buf, PAGE_SIZE, REQ_OP_WRITE)) {
			/* As flush_scrub_stripes() counts the data columns'. */
			btrfs_dev_stat_inc_and_print(tgt, BTRFS_DEV_STAT_WRITE_ERRS);
			atomic64_add(DIV_ROUND_UP(PAGE_SIZE, fs_info->sectorsize),
				     &fs_info->dev_replace.num_write_errors);
			btrfs_err_rl(fs_info,
"scrub: device replace could not write the parity of full stripe %llu to the target device; the replace will be aborted",
				     full_stripe_start);
			ret = -EIO;
			break;
		}
	}
	__free_page(page);
	if (ret < 0 || !lost)
		return ret;

	nr_lost = DIV_ROUND_UP(lost, fs_info->sectorsize);
	atomic64_add(nr_lost, &fs_info->dev_replace.num_uncorrectable_read_errors);
	why = scrub_replace_add_record(fs_info, full_stripe_start, data_stripes);
	/*
	 * Parity p is recorded in block p of the full stripe (see
	 * btrfs_wib_disk_entry::stale_par), so a stripe with fewer data
	 * columns than parities cannot say it about its last parity.  And the
	 * mark is the replace's own until it ends, as a data column's is
	 * (scrub_replace_record_lost()).
	 */
	if (!why && parity >= data_stripes)
		why = "the write-intent log cannot name that parity";
	else if (!why &&
		 !btrfs_wib_replace_mark_parity(fs_info, full_stripe_start, parity,
						!READ_ONCE(replace_keeps_marks)))
		why = "the write-intent log could not keep the record";
	btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_REPLACE_LOST,
			   full_stripe_start, NULL, 0);
	if (why) {
		btrfs_err(fs_info,
"scrub: device replace can neither regenerate nor copy %u sector(s) of parity %d of full stripe %llu, and cannot record them because %s; aborting the replace",
			  nr_lost, parity, full_stripe_start, why);
		scrub_replace_aborted(fs_info, full_stripe_start);
		return -EIO;
	}
	btrfs_err_rl(fs_info,
"scrub: device replace could neither regenerate nor copy %u sector(s) of parity %d of full stripe %llu; the target holds zeros there, recorded stale, so no rebuild uses them",
		     nr_lost, parity, full_stripe_start);
	return 0;
}

/*
 * Testing only: let a full stripe with unrepaired sectors hand
 * scrub_raid56_parity_stripe()'s caller whatever its last data column's extent
 * lookup returned, as it did before -- 1 when that column holds no extent,
 * which scrub_stripe() takes for "stop" -- so that the rest of the chunk on
 * the parity's device goes unscrubbed without a word.  The negative control
 * for uml/scrub_unrepaired_stop.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool unrepaired_stops;
module_param_named(raid56_scrub_unrepaired_stops, unrepaired_stops, bool, 0644);
MODULE_PARM_DESC(raid56_scrub_unrepaired_stops,
		 "Let a RAID5/6 full stripe with unrepaired sectors end the scrub of the rest of its chunk on the parity's device when its last data column holds no extent (testing only: restores a known defect)");
#else
static const bool unrepaired_stops;
#endif

/*
 * Testing only: leave the record of a full stripe that no longer holds any
 * extent as it is, as the scrub did before, rather than retire it: a stripe
 * the record made undecidable goes on refusing every write into it after the
 * files that held its undecidable data are deleted, and the new files meant
 * to replace them fail where they land there.  The negative control for the
 * clear arm of uml/torn_present.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool empty_keeps_record;
module_param_named(raid56_scrub_empty_keeps_record, empty_keeps_record, bool, 0644);
MODULE_PARM_DESC(raid56_scrub_empty_keeps_record,
		 "Let a RAID5/6 scrub leave the write-intent record of a full stripe that holds no extent any more, refusing writes into it for as long as it made the stripe undecidable (testing only: restores the old behaviour)");
#else
static const bool empty_keeps_record;
#endif

static int scrub_raid56_parity_stripe(struct scrub_ctx *sctx,
				      struct btrfs_device *scrub_dev,
				      struct btrfs_block_group *bg,
				      struct btrfs_chunk_map *map,
				      u64 full_stripe_start, bool regen_parity)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	BTRFS_PATH_AUTO_RELEASE(extent_path);
	BTRFS_PATH_AUTO_RELEASE(csum_path);
	struct scrub_stripe *stripe;
	bool all_empty = true;
	const int data_stripes = nr_data_stripes(map);
	const u64 fstripe_len = btrfs_stripe_nr_to_offset(data_stripes);
	unsigned long extent_bitmap = 0;
	enum scrub_wib_plan plan;
	u64 wib_holes = 0;
	int ret;

	ASSERT(sctx->raid56_data_stripes);

	ret = should_cancel_scrub(sctx);
	if (ret < 0)
		return ret;

	/*
	 * Not for the write-intent log's recovery.  It is deliberately absent
	 * from fs_info->scrubs_running (see
	 * btrfs_scrub_raid56_recovery_begin()), and scrub_blocked_if_needed()
	 * increments fs_info->scrubs_paused -- so joining the pause protocol
	 * here makes paused exceed running, and btrfs_scrub_pause() waits for
	 * the two to be EQUAL.  A transaction commit would then be held off
	 * until recovery came back out, which is the exact thing keeping
	 * recovery out of scrubs_running was meant to prevent.
	 */
	if (!sctx->internal && atomic_read(&fs_info->scrub_pause_req))
		scrub_blocked_if_needed(fs_info);

	spin_lock(&bg->lock);
	if (test_bit(BLOCK_GROUP_FLAG_REMOVED, &bg->runtime_flags)) {
		spin_unlock(&bg->lock);
		return 0;
	}
	spin_unlock(&bg->lock);

	/*
	 * For data stripe search, we cannot reuse the same extent/csum paths,
	 * as the data stripe bytenr may be smaller than previous extent.  Thus
	 * we have to use our own extent/csum paths.
	 */
	extent_path.search_commit_root = true;
	extent_path.skip_locking = true;
	csum_path.search_commit_root = true;
	csum_path.skip_locking = true;

	for (int i = 0; i < data_stripes; i++) {
		int stripe_index;
		int rot;
		u64 physical;

		stripe = &sctx->raid56_data_stripes[i];
		rot = div_u64(full_stripe_start - bg->start,
			      data_stripes) >> BTRFS_STRIPE_LEN_SHIFT;
		stripe_index = (i + rot) % map->num_stripes;
		physical = map->stripes[stripe_index].physical +
			   btrfs_stripe_nr_to_offset(rot);

		scrub_reset_stripe(stripe);
		set_bit(SCRUB_STRIPE_FLAG_NO_REPORT, &stripe->state);
		ret = scrub_find_fill_first_stripe(bg, &extent_path, &csum_path,
				map->stripes[stripe_index].dev, physical, 1,
				full_stripe_start + btrfs_stripe_nr_to_offset(i),
				BTRFS_STRIPE_LEN, stripe);
		if (ret < 0)
			return ret;
		/*
		 * No extent in this data stripe, need to manually mark them
		 * initialized to make later read submission happy.
		 */
		if (ret > 0) {
			stripe->logical = full_stripe_start +
					  btrfs_stripe_nr_to_offset(i);
			stripe->dev = map->stripes[stripe_index].dev;
			stripe->mirror_num = 1;
			set_bit(SCRUB_STRIPE_FLAG_INITIALIZED, &stripe->state);
		}
	}

	/*
	 * Decide now what the write-intent log lets us do here, while the
	 * extent and checksum information is in hand and before any read is
	 * submitted: a column we can prove stale has to be flagged before its
	 * repair worker runs.
	 */
	plan = scrub_raid56_plan_wib(sctx, map, full_stripe_start, data_stripes,
				     &wib_holes);

	/* Check if all data stripes are empty. */
	for (int i = 0; i < data_stripes; i++) {
		stripe = &sctx->raid56_data_stripes[i];
		if (!scrub_bitmap_empty_has_extent(stripe)) {
			all_empty = false;
			break;
		}
	}
	/*
	 * Nothing in it is referenced any more, so nothing its record stands
	 * for is either: the files that held data it made undecidable were
	 * deleted, typically, to be restored from a backup.  Left in place, a
	 * record naming more than the parity can rebuild refuses every write
	 * into the stripe, the restored files' included where they land there.
	 * Retire it, as the parity regenerated below would: whatever a later
	 * write puts there, it computes the parity of its rows from the data.
	 * Not for the recovery, which decides from the record, nor for a
	 * replace or a read-only scrub.
	 */
	if (all_empty) {
		if (regen_parity && !sctx->readonly && !sctx->internal &&
		    !sctx->is_dev_replace && !sctx->raid56_defer_retire &&
		    !READ_ONCE(empty_keeps_record) && btrfs_wib_any_stale(fs_info))
			btrfs_wib_clear_sticky(fs_info, full_stripe_start, fstripe_len);
		return 0;
	}

	for (int i = 0; i < data_stripes; i++) {
		stripe = &sctx->raid56_data_stripes[i];
		scrub_submit_initial_read(sctx, stripe);
	}
	for (int i = 0; i < data_stripes; i++) {
		stripe = &sctx->raid56_data_stripes[i];

		wait_event(stripe->repair_wait,
			   test_bit(SCRUB_STRIPE_FLAG_REPAIR_DONE, &stripe->state));
	}
	/* For now, no zoned support for RAID56. */
	ASSERT(!btrfs_is_zoned(sctx->fs_info));

	/*
	 * Genuinely ambiguous: the log names members it cannot vouch for and
	 * there is not enough good parity left to rebuild them.  Recomputing
	 * the parity here would destroy the only surviving copy of what was
	 * acknowledged, and rebuilding the data would invent a value nothing
	 * ever committed.  Do neither.  Leave every byte as it is, keep the
	 * record, and say so loudly enough that a recovery tool -- and a human
	 * -- can pick it up; btrfs_wib_stripe_state() still describes exactly
	 * which members are named and which are merely suspect.
	 *
	 * Ahead of the unrepaired-sector check below, because that check now
	 * fires on the very sectors this condition explains.
	 * scrub_note_unprovable() restores the error bit of a reconstruction
	 * nothing could verify, and those sectors have extents, so the generic
	 * "unrepaired sectors detected" would win the race to report and would
	 * be true but useless: it names no member, and it increments no counter
	 * a recovery helper can enumerate.  Both paths leave every byte of the
	 * stripe alone and keep the record -- only the diagnosis differs, and
	 * this one is the specific one.
	 */
	if (regen_parity &&
	    (plan == SCRUB_WIB_AMBIGUOUS || plan == SCRUB_WIB_TORN)) {
		sctx->raid56_keep_record = true;
		scrub_capture_evidence(sctx, map, bg, full_stripe_start,
				       data_stripes);
		atomic64_inc(&fs_info->wib->stat_scrub_skipped_stale);
		if (plan == SCRUB_WIB_TORN) {
			sctx->raid56_torn_undecided = true;
			btrfs_warn_rl(fs_info,
"scrub: full stripe %llu left untouched: a write into it may have been torn, and no parity is left over to check the rebuild of the %u data stripe(s) whose last write did not reach the disk -- keeping the record rather than guessing",
				      full_stripe_start,
				      (unsigned int)hweight64(wib_holes));
			/*
			 * Undecidable, and more than a warning should say so.
			 * The recovery's verdict raises the alert where it
			 * records the stripe (scrub_raid56_mark_suspect()).
			 */
			if (!sctx->internal && !btrfs_wib_torn_remedy_legacy())
				btrfs_raid56_alert(fs_info,
						   BTRFS_RAID56_EV_TORN_UNDECIDABLE,
						   full_stripe_start, NULL, 0);
		} else {
			btrfs_warn_rl(fs_info,
"scrub: full stripe %llu left untouched: %u data stripe(s) whose last write did not reach the disk cannot be rebuilt from the parity that is left, and without a checksum there is nothing to decide it with -- keeping the record rather than guessing",
				      full_stripe_start,
				      (unsigned int)hweight64(wib_holes));
		}
		/* Untouched, but a replace target still needs the parity. */
		if (sctx->raid56_whole_column)
			return scrub_replace_copy_parity(sctx, scrub_dev, bg, map,
							 full_stripe_start);
		return 0;
	}

	/*
	 * Now all data stripes are properly verified. Check if we have any
	 * unrepaired, if so abort immediately or we could further corrupt the
	 * P/Q stripes.
	 *
	 * During the loop, also populate extent_bitmap.
	 */
	for (int i = 0; i < data_stripes; i++) {
		unsigned long error;
		unsigned long has_extent;

		stripe = &sctx->raid56_data_stripes[i];

		error = scrub_bitmap_read_error(stripe);
		has_extent = scrub_bitmap_read_has_extent(stripe);

		/*
		 * We should only check the errors where there is an extent.
		 * As we may hit an empty data stripe while it's missing.
		 */
		bitmap_and(&error, &error, &has_extent, stripe->nr_sectors);
		if (unlikely(!bitmap_empty(&error, stripe->nr_sectors))) {
			btrfs_err(fs_info,
"scrub: unrepaired sectors detected, full stripe %llu data stripe %u errors %*pbl",
				  full_stripe_start, i, stripe->nr_sectors,
				  &error);
			sctx->raid56_keep_record = true;
			/*
			 * A replace still needs this parity on the target.  And
			 * @ret is whatever the last column's extent lookup
			 * returned: 1 when it found none, which scrub_stripe()
			 * takes for "stop", and the rest of the chunk was never
			 * copied -- nor, for a scrub, scrubbed, with the scrub
			 * reporting success.  This stripe is reported above and
			 * keeps its record; the next one is not its business.
			 */
			if (sctx->raid56_whole_column)
				return scrub_replace_copy_parity(sctx, scrub_dev,
								 bg, map,
								 full_stripe_start);
			return READ_ONCE(unrepaired_stops) ? ret : 0;
		}
		bitmap_or(&extent_bitmap, &extent_bitmap, &has_extent,
			  stripe->nr_sectors);
	}

	if (!regen_parity)
		return 0;

	/* Now we can check and regenerate the P/Q stripe. */
	ret = scrub_raid56_cached_parity(sctx, scrub_dev, map, full_stripe_start,
					 &extent_bitmap);
	if (ret < 0)
		return ret;

	/*
	 * Retire the record only if the repair actually reached the platter.
	 *
	 * A failed repair write is invisible to everything above: it lands in
	 * stripe->write_error_bitmap, which is not one of the scrub bitmaps
	 * and which the unrepaired-sectors check above never reads -- the
	 * reconstruction cleared those bits by succeeding in memory.  And the
	 * parity written just now was computed from that same memory
	 * (raid56_parity_cache_data_folios()), so it describes the repaired
	 * value whether or not the disk ever received it.  Clearing the record
	 * on that basis would forget the one stripe most in need of another
	 * look, on the strength of a repair that did not happen.
	 *
	 * btrfs_scrub_raid56_full_stripe() already folds this bitmap into its
	 * result for the recovery caller; the check simply was not on the user
	 * scrub's path.
	 *
	 * Not while read-only either: the write-back is skipped entirely then,
	 * so the sectors this claims to have repaired are still stale and the
	 * record is the only thing that knows.
	 */
	if (sctx->readonly) {
		sctx->raid56_keep_record = true;
		return 0;
	}
	for (int i = 0; i < data_stripes; i++) {
		if (sctx->raid56_data_stripes[i].write_error_bitmap) {
			btrfs_warn_rl(fs_info,
"scrub: full stripe %llu was repaired in memory but a write-back failed; keeping its record",
				      full_stripe_start);
			sctx->raid56_keep_record = true;
			return 0;
		}
	}
	if (!sctx->raid56_defer_retire)
		btrfs_wib_clear_sticky(fs_info, full_stripe_start, fstripe_len);
	return 0;
}

/*
 * Scrub one range which can only has simple mirror based profile.
 * (Including all range in SINGLE/DUP/RAID1/RAID1C*, and each stripe in
 *  RAID0/RAID10).
 *
 * Since we may need to handle a subset of block group, we need @logical_start
 * and @logical_length parameter.
 */
static int scrub_simple_mirror(struct scrub_ctx *sctx,
			       struct btrfs_block_group *bg,
			       u64 logical_start, u64 logical_length,
			       struct btrfs_device *device,
			       u64 physical, int mirror_num)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	const u64 logical_end = logical_start + logical_length;
	u64 cur_logical = logical_start;
	int ret = 0;

	/* The range must be inside the bg */
	ASSERT(logical_start >= bg->start && logical_end <= btrfs_block_group_end(bg));

	/* Go through each extent items inside the logical range */
	while (cur_logical < logical_end) {
		u64 found_logical = U64_MAX;
		u64 cur_physical = physical + cur_logical - logical_start;

		ret = should_cancel_scrub(sctx);
		if (ret < 0)
			break;

		if (atomic_read(&fs_info->scrub_pause_req))
			scrub_blocked_if_needed(fs_info);

		spin_lock(&bg->lock);
		if (test_bit(BLOCK_GROUP_FLAG_REMOVED, &bg->runtime_flags)) {
			spin_unlock(&bg->lock);
			ret = 0;
			break;
		}
		spin_unlock(&bg->lock);

		ret = queue_scrub_stripe(sctx, bg, device, mirror_num,
					 cur_logical, logical_end - cur_logical,
					 cur_physical, &found_logical);
		if (ret > 0) {
			/* No more extent, just update the accounting */
			spin_lock(&sctx->stat_lock);
			sctx->stat.last_physical = physical + logical_length;
			spin_unlock(&sctx->stat_lock);
			ret = 0;
			break;
		}
		if (ret < 0)
			break;

		/* queue_scrub_stripe() returned 0, @found_logical must be updated. */
		ASSERT(found_logical != U64_MAX);
		cur_logical = found_logical + BTRFS_STRIPE_LEN;

		/* Don't hold CPU for too long time */
		cond_resched();
	}
	return ret;
}

/* Calculate the full stripe length for simple stripe based profiles */
static u64 simple_stripe_full_stripe_len(const struct btrfs_chunk_map *map)
{
	ASSERT(map->type & (BTRFS_BLOCK_GROUP_RAID0 |
			    BTRFS_BLOCK_GROUP_RAID10));

	return btrfs_stripe_nr_to_offset(map->num_stripes / map->sub_stripes);
}

/* Get the logical bytenr for the stripe */
static u64 simple_stripe_get_logical(struct btrfs_chunk_map *map,
				     struct btrfs_block_group *bg,
				     int stripe_index)
{
	ASSERT(map->type & (BTRFS_BLOCK_GROUP_RAID0 |
			    BTRFS_BLOCK_GROUP_RAID10));
	ASSERT(stripe_index < map->num_stripes);

	/*
	 * (stripe_index / sub_stripes) gives how many data stripes we need to
	 * skip.
	 */
	return btrfs_stripe_nr_to_offset(stripe_index / map->sub_stripes) +
	       bg->start;
}

/* Get the mirror number for the stripe */
static int simple_stripe_mirror_num(struct btrfs_chunk_map *map, int stripe_index)
{
	ASSERT(map->type & (BTRFS_BLOCK_GROUP_RAID0 |
			    BTRFS_BLOCK_GROUP_RAID10));
	ASSERT(stripe_index < map->num_stripes);

	/* For RAID0, it's fixed to 1, for RAID10 it's 0,1,0,1... */
	return stripe_index % map->sub_stripes + 1;
}

static int scrub_simple_stripe(struct scrub_ctx *sctx,
			       struct btrfs_block_group *bg,
			       struct btrfs_chunk_map *map,
			       struct btrfs_device *device,
			       int stripe_index)
{
	const u64 logical_increment = simple_stripe_full_stripe_len(map);
	const u64 orig_logical = simple_stripe_get_logical(map, bg, stripe_index);
	const u64 orig_physical = map->stripes[stripe_index].physical;
	const u64 end = btrfs_block_group_end(bg);
	const int mirror_num = simple_stripe_mirror_num(map, stripe_index);
	u64 cur_logical = orig_logical;
	u64 cur_physical = orig_physical;
	int ret = 0;

	while (cur_logical < end) {
		/*
		 * Inside each stripe, RAID0 is just SINGLE, and RAID10 is
		 * just RAID1, so we can reuse scrub_simple_mirror() to scrub
		 * this stripe.
		 */
		ret = scrub_simple_mirror(sctx, bg, cur_logical,
					  BTRFS_STRIPE_LEN, device, cur_physical,
					  mirror_num);
		if (ret)
			return ret;
		/* Skip to next stripe which belongs to the target device */
		cur_logical += logical_increment;
		/* For physical offset, we just go to next stripe */
		cur_physical += BTRFS_STRIPE_LEN;
	}
	return ret;
}

static noinline_for_stack int scrub_stripe(struct scrub_ctx *sctx,
					   struct btrfs_block_group *bg,
					   struct btrfs_chunk_map *map,
					   struct btrfs_device *scrub_dev,
					   int stripe_index)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	const u64 profile = map->type & BTRFS_BLOCK_GROUP_PROFILE_MASK;
	const u64 chunk_logical = bg->start;
	int ret;
	int ret2;
	u64 physical = map->stripes[stripe_index].physical;
	const u64 dev_stripe_len = btrfs_calc_stripe_length(map);
	const u64 physical_end = physical + dev_stripe_len;
	u64 logical;
	u64 logic_end;
	/* The logical increment after finishing one stripe */
	u64 increment;
	/* Offset inside the chunk */
	u64 offset;
	u64 stripe_logical;

	/* Extent_path should be released by now. */
	ASSERT(sctx->extent_path.nodes[0] == NULL);

	scrub_blocked_if_needed(fs_info);

	if (sctx->is_dev_replace &&
	    btrfs_dev_is_sequential(sctx->wr_tgtdev, physical)) {
		mutex_lock(&sctx->wr_lock);
		sctx->write_pointer = physical;
		mutex_unlock(&sctx->wr_lock);
	}

	/* Prepare the extra data stripes used by RAID56. */
	if (profile & BTRFS_BLOCK_GROUP_RAID56_MASK) {
		ASSERT(sctx->raid56_data_stripes == NULL);

		sctx->raid56_data_stripes = kzalloc_objs(struct scrub_stripe,
							 nr_data_stripes(map));
		if (!sctx->raid56_data_stripes) {
			ret = -ENOMEM;
			goto out;
		}
		sctx->nr_raid56_data_stripes = nr_data_stripes(map);
		for (int i = 0; i < nr_data_stripes(map); i++) {
			ret = init_scrub_stripe(fs_info,
						&sctx->raid56_data_stripes[i]);
			if (ret < 0)
				goto out;
			sctx->raid56_data_stripes[i].bg = bg;
			sctx->raid56_data_stripes[i].sctx = sctx;
		}
	}
	/*
	 * There used to be a big double loop to handle all profiles using the
	 * same routine, which grows larger and more gross over time.
	 *
	 * So here we handle each profile differently, so simpler profiles
	 * have simpler scrubbing function.
	 */
	if (!(profile & (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID10 |
			 BTRFS_BLOCK_GROUP_RAID56_MASK))) {
		/*
		 * Above check rules out all complex profile, the remaining
		 * profiles are SINGLE|DUP|RAID1|RAID1C*, which is simple
		 * mirrored duplication without stripe.
		 *
		 * Only @physical and @mirror_num needs to calculated using
		 * @stripe_index.
		 */
		ret = scrub_simple_mirror(sctx, bg, bg->start, bg->length,
				scrub_dev, map->stripes[stripe_index].physical,
				stripe_index + 1);
		offset = 0;
		goto out;
	}
	if (profile & (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID10)) {
		ret = scrub_simple_stripe(sctx, bg, map, scrub_dev, stripe_index);
		offset = btrfs_stripe_nr_to_offset(stripe_index / map->sub_stripes);
		goto out;
	}

	/* Only RAID56 goes through the old code */
	ASSERT(map->type & BTRFS_BLOCK_GROUP_RAID56_MASK);
	ret = 0;

	/* Calculate the logical end of the stripe */
	get_raid56_logic_offset(physical_end, stripe_index,
				map, &logic_end, NULL);
	logic_end += chunk_logical;

	/* Initialize @offset in case we need to go to out: label */
	get_raid56_logic_offset(physical, stripe_index, map, &offset, NULL);
	increment = btrfs_stripe_nr_to_offset(nr_data_stripes(map));

	/*
	 * Due to the rotation, for RAID56 it's better to iterate each stripe
	 * using their physical offset.
	 */
	while (physical < physical_end) {
		ret = get_raid56_logic_offset(physical, stripe_index, map,
					      &logical, &stripe_logical);
		logical += chunk_logical;
		if (ret) {
			/* it is parity strip */
			stripe_logical += chunk_logical;
			ret = scrub_raid56_parity_stripe(sctx, scrub_dev, bg,
							 map, stripe_logical, true);
			spin_lock(&sctx->stat_lock);
			sctx->stat.last_physical = min(physical + BTRFS_STRIPE_LEN,
						       physical_end);
			spin_unlock(&sctx->stat_lock);
			if (ret)
				goto out;
			goto next;
		}

		/*
		 * Now we're at a data stripe, scrub each extents in the range.
		 *
		 * At this stage, if we ignore the repair part, inside each data
		 * stripe it is no different than SINGLE profile.
		 * We can reuse scrub_simple_mirror() here, as the repair part
		 * is still based on @mirror_num.
		 *
		 * Except for a replace, which has to give the target the whole
		 * column of every full stripe that holds anything, not only
		 * the sectors of this column an extent covers.
		 */
		if (sctx->raid56_whole_column)
			ret = queue_raid56_replace_column(sctx, bg, scrub_dev,
							  logical, physical,
							  stripe_logical + chunk_logical,
							  nr_data_stripes(map));
		else
			ret = scrub_simple_mirror(sctx, bg, logical, BTRFS_STRIPE_LEN,
						  scrub_dev, physical, 1);
		if (ret < 0)
			goto out;
next:
		logical += increment;
		physical += BTRFS_STRIPE_LEN;
		spin_lock(&sctx->stat_lock);
		sctx->stat.last_physical = physical;
		spin_unlock(&sctx->stat_lock);
	}
out:
	ret2 = flush_scrub_stripes(sctx);
	if (!ret)
		ret = ret2;
	btrfs_release_path(&sctx->extent_path);
	btrfs_release_path(&sctx->csum_path);

	scrub_free_raid56_data_stripes(sctx);

	if (sctx->is_dev_replace && ret >= 0) {
		ret2 = sync_write_pointer_for_zoned(sctx,
				chunk_logical + offset,
				map->stripes[stripe_index].physical,
				physical_end);
		if (ret2)
			ret = ret2;
	}

	return ret < 0 ? ret : 0;
}

static noinline_for_stack int scrub_chunk(struct scrub_ctx *sctx,
					  struct btrfs_block_group *bg,
					  struct btrfs_device *scrub_dev,
					  u64 dev_offset,
					  u64 dev_extent_len)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct btrfs_chunk_map *map;
	int i;
	int ret = 0;

	map = btrfs_find_chunk_map(fs_info, bg->start, bg->length);
	if (!map) {
		/*
		 * Might have been an unused block group deleted by the cleaner
		 * kthread or relocation.
		 */
		spin_lock(&bg->lock);
		if (!test_bit(BLOCK_GROUP_FLAG_REMOVED, &bg->runtime_flags))
			ret = -EINVAL;
		spin_unlock(&bg->lock);

		return ret;
	}
	if (map->start != bg->start)
		goto out;
	if (map->chunk_len < dev_extent_len)
		goto out;

	for (i = 0; i < map->num_stripes; ++i) {
		if (map->stripes[i].dev->bdev == scrub_dev->bdev &&
		    map->stripes[i].physical == dev_offset) {
			ret = scrub_stripe(sctx, bg, map, scrub_dev, i);
			if (ret)
				goto out;
		}
	}
out:
	btrfs_free_chunk_map(map);

	return ret;
}

/*
 * Wait for every write already under way into @cache, which is read-only by
 * now, and commit: afterwards everything allocated in it is on disk and in the
 * commit root the copy looks extents up in, and nothing new can be.
 */
static int finish_extent_writes(struct btrfs_root *root,
				struct btrfs_block_group *cache)
{
	struct btrfs_fs_info *fs_info = cache->fs_info;

	btrfs_wait_block_group_reservations(cache);
	btrfs_wait_nocow_writers(cache);
	btrfs_wait_ordered_roots(fs_info, U64_MAX, cache);

	return btrfs_commit_current_transaction(root);
}

static int finish_extent_writes_for_zoned(struct btrfs_root *root,
					  struct btrfs_block_group *cache)
{
	if (!btrfs_is_zoned(cache->fs_info))
		return 0;

	return finish_extent_writes(root, cache);
}

static noinline_for_stack
int scrub_enumerate_chunks(struct scrub_ctx *sctx,
			   struct btrfs_device *scrub_dev, u64 start, u64 end)
{
	struct btrfs_dev_extent *dev_extent = NULL;
	BTRFS_PATH_AUTO_FREE(path);
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct btrfs_root *root = fs_info->dev_root;
	u64 chunk_offset;
	int ret = 0;
	int ro_set;
	int slot;
	struct extent_buffer *l;
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_block_group *cache;
	struct btrfs_dev_replace *dev_replace = &fs_info->dev_replace;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->reada = READA_FORWARD;
	path->search_commit_root = true;
	path->skip_locking = true;

	key.objectid = scrub_dev->devid;
	key.type = BTRFS_DEV_EXTENT_KEY;
	key.offset = 0ull;

	while (1) {
		u64 dev_extent_len;

		ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
		if (ret < 0)
			break;
		if (ret > 0) {
			if (path->slots[0] >=
			    btrfs_header_nritems(path->nodes[0])) {
				ret = btrfs_next_leaf(root, path);
				if (ret < 0)
					break;
				if (ret > 0) {
					ret = 0;
					break;
				}
			} else {
				ret = 0;
			}
		}

		l = path->nodes[0];
		slot = path->slots[0];

		btrfs_item_key_to_cpu(l, &found_key, slot);

		if (found_key.objectid != scrub_dev->devid)
			break;

		if (found_key.type != BTRFS_DEV_EXTENT_KEY)
			break;

		if (found_key.offset >= end)
			break;

		if (found_key.offset < key.offset)
			break;

		dev_extent = btrfs_item_ptr(l, slot, struct btrfs_dev_extent);
		dev_extent_len = btrfs_dev_extent_length(l, dev_extent);

		if (found_key.offset + dev_extent_len <= start)
			goto skip;

		chunk_offset = btrfs_dev_extent_chunk_offset(l, dev_extent);

		/*
		 * get a reference on the corresponding block group to prevent
		 * the chunk from going away while we scrub it
		 */
		cache = btrfs_lookup_block_group(fs_info, chunk_offset);

		/* some chunks are removed but not committed to disk yet,
		 * continue scrubbing */
		if (!cache)
			goto skip;

		ASSERT(cache->start <= chunk_offset);
		/*
		 * We are using the commit root to search for device extents, so
		 * that means we could have found a device extent item from a
		 * block group that was deleted in the current transaction. The
		 * logical start offset of the deleted block group, stored at
		 * @chunk_offset, might be part of the logical address range of
		 * a new block group (which uses different physical extents).
		 * In this case btrfs_lookup_block_group() has returned the new
		 * block group, and its start address is less than @chunk_offset.
		 *
		 * We skip such new block groups, because it's pointless to
		 * process them, as we won't find their extents because we search
		 * for them using the commit root of the extent tree. For a device
		 * replace it's also fine to skip it, we won't miss copying them
		 * to the target device because we have the write duplication
		 * setup through the regular write path (by btrfs_map_block()),
		 * and we have committed a transaction when we started the device
		 * replace, right after setting up the device replace state.
		 */
		if (cache->start < chunk_offset) {
			btrfs_put_block_group(cache);
			goto skip;
		}

		if (sctx->is_dev_replace && btrfs_is_zoned(fs_info)) {
			if (!test_bit(BLOCK_GROUP_FLAG_TO_COPY, &cache->runtime_flags)) {
				btrfs_put_block_group(cache);
				goto skip;
			}
		}

		/*
		 * Make sure that while we are scrubbing the corresponding block
		 * group doesn't get its logical address and its device extents
		 * reused for another block group, which can possibly be of a
		 * different type and different profile. We do this to prevent
		 * false error detections and crashes due to bogus attempts to
		 * repair extents.
		 */
		spin_lock(&cache->lock);
		if (test_bit(BLOCK_GROUP_FLAG_REMOVED, &cache->runtime_flags)) {
			spin_unlock(&cache->lock);
			btrfs_put_block_group(cache);
			goto skip;
		}
		btrfs_freeze_block_group(cache);
		spin_unlock(&cache->lock);

		/*
		 * we need call btrfs_inc_block_group_ro() with scrubs_paused,
		 * to avoid deadlock caused by:
		 * btrfs_inc_block_group_ro()
		 * -> btrfs_wait_for_commit()
		 * -> btrfs_commit_transaction()
		 * -> btrfs_scrub_pause()
		 */
		scrub_pause_on(fs_info);

		/*
		 * Don't do chunk preallocation for scrub.
		 *
		 * This is especially important for SYSTEM bgs, or we can hit
		 * -EFBIG from btrfs_finish_chunk_alloc() like:
		 * 1. The only SYSTEM bg is marked RO.
		 *    Since SYSTEM bg is small, that's pretty common.
		 * 2. New SYSTEM bg will be allocated
		 *    Due to regular version will allocate new chunk.
		 * 3. New SYSTEM bg is empty and will get cleaned up
		 *    Before cleanup really happens, it's marked RO again.
		 * 4. Empty SYSTEM bg get scrubbed
		 *    We go back to 2.
		 *
		 * This can easily boost the amount of SYSTEM chunks if cleaner
		 * thread can't be triggered fast enough, and use up all space
		 * of btrfs_super_block::sys_chunk_array
		 *
		 * While for dev replace, we need to try our best to mark block
		 * group RO, to prevent race between:
		 * - Write duplication
		 *   Contains latest data
		 * - Scrub copy
		 *   Contains data from commit tree
		 *
		 * If target block group is not marked RO, nocow writes can
		 * be overwritten by scrub copy, causing data corruption.
		 * So for dev-replace, it's not allowed to continue if a block
		 * group is not RO.
		 */
		ret = btrfs_inc_block_group_ro(cache, sctx->is_dev_replace);
		/*
		 * A replace copying whole RAID5/6 columns writes the target's
		 * free sectors too -- free as the commit root has it.  A tree
		 * block allocated here in the running transaction, before the
		 * block group went read-only, sits in such a sector and is
		 * written out at the commit, to the source and, duplicated, to
		 * the target: possibly between the copy's read of that row and
		 * its write, which would then put the old content back over it
		 * on the target.  The same goes for data written since the last
		 * commit, which the copy would rebuild instead of verifying.
		 * Committing now, with nothing more to come into the block
		 * group, leaves the copy nothing it does not see.
		 */
		sctx->raid56_whole_column = scrub_replace_copies_column(sctx, cache);
		if (!ret && sctx->is_dev_replace) {
			ret = finish_extent_writes_for_zoned(root, cache);
			if (!ret && sctx->raid56_whole_column)
				ret = finish_extent_writes(root, cache);
			if (ret) {
				btrfs_dec_block_group_ro(cache);
				scrub_pause_off(fs_info);
				btrfs_put_block_group(cache);
				break;
			}
		}

		if (ret == 0) {
			ro_set = 1;
		} else if (ret == -ENOSPC && !sctx->is_dev_replace &&
			   !(cache->flags & BTRFS_BLOCK_GROUP_RAID56_MASK)) {
			/*
			 * btrfs_inc_block_group_ro return -ENOSPC when it
			 * failed in creating new chunk for metadata.
			 * It is not a problem for scrub, because
			 * metadata are always cowed, and our scrub paused
			 * commit_transactions.
			 *
			 * For RAID56 chunks, we have to mark them read-only
			 * for scrub, as later we would use our own cache
			 * out of RAID56 realm.
			 * Thus we want the RAID56 bg to be marked RO to
			 * prevent RMW from screwing up out cache.
			 */
			ro_set = 0;
		} else if (ret == -ETXTBSY) {
			btrfs_warn(fs_info,
	     "scrub: skipping scrub of block group %llu due to active swapfile",
				   cache->start);
			scrub_pause_off(fs_info);
			ret = 0;
			goto skip_unfreeze;
		} else {
			btrfs_warn(fs_info, "scrub: failed setting block group ro: %d",
				   ret);
			btrfs_unfreeze_block_group(cache);
			btrfs_put_block_group(cache);
			scrub_pause_off(fs_info);
			break;
		}

		/*
		 * Now the target block is marked RO, wait for nocow writes to
		 * finish before dev-replace.
		 * COW is fine, as COW never overwrites extents in commit tree.
		 */
		if (sctx->is_dev_replace) {
			btrfs_wait_nocow_writers(cache);
			btrfs_wait_ordered_roots(fs_info, U64_MAX, cache);
		}

		scrub_pause_off(fs_info);
		down_write(&dev_replace->rwsem);
		dev_replace->cursor_right = found_key.offset + dev_extent_len;
		dev_replace->cursor_left = found_key.offset;
		dev_replace->item_needs_writeback = 1;
		up_write(&dev_replace->rwsem);

		ret = scrub_chunk(sctx, cache, scrub_dev, found_key.offset,
				  dev_extent_len);
		if (sctx->is_dev_replace &&
		    !btrfs_finish_block_group_to_copy(dev_replace->srcdev,
						      cache, found_key.offset))
			ro_set = 0;

		down_write(&dev_replace->rwsem);
		dev_replace->cursor_left = dev_replace->cursor_right;
		dev_replace->item_needs_writeback = 1;
		up_write(&dev_replace->rwsem);

		if (ro_set)
			btrfs_dec_block_group_ro(cache);

		/*
		 * We might have prevented the cleaner kthread from deleting
		 * this block group if it was already unused because we raced
		 * and set it to RO mode first. So add it back to the unused
		 * list, otherwise it might not ever be deleted unless a manual
		 * balance is triggered or it becomes used and unused again.
		 */
		spin_lock(&cache->lock);
		if (!test_bit(BLOCK_GROUP_FLAG_REMOVED, &cache->runtime_flags) &&
		    !cache->ro && cache->reserved == 0 && cache->used == 0) {
			spin_unlock(&cache->lock);
			if (btrfs_test_opt(fs_info, DISCARD_ASYNC))
				btrfs_discard_queue_work(&fs_info->discard_ctl,
							 cache);
			else
				btrfs_mark_bg_unused(cache);
		} else {
			spin_unlock(&cache->lock);
		}
skip_unfreeze:
		btrfs_unfreeze_block_group(cache);
		btrfs_put_block_group(cache);
		if (ret)
			break;
		if (unlikely(sctx->is_dev_replace &&
			     atomic64_read(&dev_replace->num_write_errors) > 0)) {
			ret = -EIO;
			break;
		}
		if (sctx->stat.malloc_errors > 0) {
			ret = -ENOMEM;
			break;
		}
skip:
		key.offset = found_key.offset + dev_extent_len;
		btrfs_release_path(path);
	}

	return ret;
}

static int scrub_one_super(struct scrub_ctx *sctx, struct btrfs_device *dev,
			   struct page *page, u64 physical, u64 generation)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct btrfs_super_block *sb = page_address(page);
	int ret;

	ret = bdev_rw_virt(dev->bdev, physical >> SECTOR_SHIFT, sb,
			BTRFS_SUPER_INFO_SIZE, REQ_OP_READ);
	if (ret < 0)
		return ret;
	ret = btrfs_check_super_csum(fs_info, sb);
	if (unlikely(ret != 0)) {
		btrfs_err_rl(fs_info,
		  "scrub: super block at physical %llu devid %llu has bad csum",
			physical, dev->devid);
		return -EIO;
	}
	if (unlikely(btrfs_super_generation(sb) != generation)) {
		btrfs_err_rl(fs_info,
"scrub: super block at physical %llu devid %llu has bad generation %llu expect %llu",
			     physical, dev->devid,
			     btrfs_super_generation(sb), generation);
		return -EUCLEAN;
	}

	return btrfs_validate_super(fs_info, sb, -1);
}

static noinline_for_stack int scrub_supers(struct scrub_ctx *sctx,
					   struct btrfs_device *scrub_dev)
{
	int	i;
	u64	bytenr;
	u64	gen;
	int ret = 0;
	struct page *page;
	struct btrfs_fs_info *fs_info = sctx->fs_info;

	if (unlikely(BTRFS_FS_ERROR(fs_info)))
		return -EROFS;

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		spin_lock(&sctx->stat_lock);
		sctx->stat.malloc_errors++;
		spin_unlock(&sctx->stat_lock);
		return -ENOMEM;
	}

	/* Seed devices of a new filesystem has their own generation. */
	if (scrub_dev->fs_devices != fs_info->fs_devices)
		gen = scrub_dev->generation;
	else
		gen = btrfs_get_last_trans_committed(fs_info);

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		ret = btrfs_sb_log_location(scrub_dev, i, 0, &bytenr);
		if (ret == -ENOENT)
			break;

		if (ret) {
			spin_lock(&sctx->stat_lock);
			sctx->stat.super_errors++;
			spin_unlock(&sctx->stat_lock);
			continue;
		}

		if (bytenr + BTRFS_SUPER_INFO_SIZE >
		    scrub_dev->commit_total_bytes)
			break;
		if (!btrfs_check_super_location(scrub_dev, bytenr))
			continue;

		ret = scrub_one_super(sctx, scrub_dev, page, bytenr, gen);
		if (ret) {
			spin_lock(&sctx->stat_lock);
			sctx->stat.super_errors++;
			spin_unlock(&sctx->stat_lock);
		}
	}
	__free_page(page);
	return 0;
}

static void scrub_workers_put(struct btrfs_fs_info *fs_info)
{
	if (refcount_dec_and_mutex_lock(&fs_info->scrub_workers_refcnt,
					&fs_info->scrub_lock)) {
		struct workqueue_struct *scrub_workers = fs_info->scrub_workers;

		fs_info->scrub_workers = NULL;
		mutex_unlock(&fs_info->scrub_lock);

		if (scrub_workers)
			destroy_workqueue(scrub_workers);
	}
}

/*
 * get a reference count on fs_info->scrub_workers. start worker if necessary
 */
static noinline_for_stack int scrub_workers_get(struct btrfs_fs_info *fs_info)
{
	struct workqueue_struct *scrub_workers = NULL;
	unsigned int flags = WQ_FREEZABLE | WQ_UNBOUND;
	int max_active = fs_info->thread_pool_size;
	int ret = -ENOMEM;

	if (refcount_inc_not_zero(&fs_info->scrub_workers_refcnt))
		return 0;

	scrub_workers = alloc_workqueue("btrfs-scrub", flags, max_active);
	if (!scrub_workers)
		return -ENOMEM;

	mutex_lock(&fs_info->scrub_lock);
	if (refcount_read(&fs_info->scrub_workers_refcnt) == 0) {
		ASSERT(fs_info->scrub_workers == NULL);
		fs_info->scrub_workers = scrub_workers;
		refcount_set(&fs_info->scrub_workers_refcnt, 1);
		mutex_unlock(&fs_info->scrub_lock);
		return 0;
	}
	/* Other thread raced in and created the workers for us */
	refcount_inc(&fs_info->scrub_workers_refcnt);
	mutex_unlock(&fs_info->scrub_lock);

	ret = 0;

	destroy_workqueue(scrub_workers);
	return ret;
}

/*
 * Recompute the parity held by @scrub_dev for every vertical stripe of the
 * full stripe at @full_stripe_start from the data as it is on disk, and
 * write back the sectors that differ.  Unlike scrub_raid56_cached_parity()
 * nothing is taken from the scrub stripes: every sector is read.
 *
 * Only correct if every data sector holds what was last written to it,
 * which recovery of the write-intent log establishes before calling this.
 */
static int scrub_raid56_full_parity(struct scrub_ctx *sctx,
				    struct btrfs_device *scrub_dev,
				    struct btrfs_chunk_map *map,
				    u64 full_stripe_start)
{
	DECLARE_COMPLETION_ONSTACK(io_done);
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	struct btrfs_io_context *bioc = NULL;
	struct btrfs_raid_bio *rbio;
	struct bio bio;
	const int data_stripes = nr_data_stripes(map);
	const unsigned int nsectors = BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits;
	unsigned long full_bitmap = 0;
	u64 length = btrfs_stripe_nr_to_offset(data_stripes);
	int ret;

	ASSERT(nsectors <= BITS_PER_LONG);
	bitmap_set(&full_bitmap, 0, nsectors);

	bio_init(&bio, NULL, NULL, 0, REQ_OP_READ);
	bio.bi_iter.bi_sector = full_stripe_start >> SECTOR_SHIFT;
	bio.bi_private = &io_done;
	bio.bi_end_io = raid56_scrub_wait_endio;

	btrfs_bio_counter_inc_blocked(fs_info);
	ret = btrfs_map_block(fs_info, BTRFS_MAP_WRITE, full_stripe_start,
			      &length, &bioc, NULL, NULL);
	if (ret < 0)
		goto out;
	ASSERT(bioc);
	rbio = raid56_parity_alloc_scrub_rbio(&bio, bioc, scrub_dev, &full_bitmap,
					      nsectors);
	btrfs_put_bioc(bioc);
	if (!rbio) {
		ret = -ENOMEM;
		goto out;
	}
	raid56_parity_scrub_rbio_strict(rbio);
	raid56_parity_submit_scrub_rbio(rbio);
	wait_for_completion_io(&io_done);
	ret = blk_status_to_errno(bio.bi_status);
out:
	btrfs_bio_counter_dec(fs_info);
	bio_uninit(&bio);
	return ret;
}

/*
 * Return the geometry of the RAID56 full stripe containing @logical, or
 * -ENOENT if @logical is not inside a RAID56 block group.
 */
int btrfs_raid56_full_stripe_range(struct btrfs_fs_info *fs_info, u64 logical,
				   u64 *full_stripe_start, u64 *full_stripe_len)
{
	struct btrfs_block_group *bg;
	struct btrfs_chunk_map *map;
	u64 fstripe_len;
	int ret = 0;

	bg = btrfs_lookup_block_group(fs_info, logical);
	if (!bg)
		return -ENOENT;
	if (!(bg->flags & BTRFS_BLOCK_GROUP_RAID56_MASK)) {
		ret = -ENOENT;
		goto out;
	}
	map = btrfs_find_chunk_map(fs_info, bg->start, bg->length);
	if (!map) {
		ret = -ENOENT;
		goto out;
	}
	fstripe_len = btrfs_stripe_nr_to_offset(nr_data_stripes(map));
	*full_stripe_start = bg->start +
			     div_u64(logical - bg->start, fstripe_len) * fstripe_len;
	*full_stripe_len = fstripe_len;
	btrfs_free_chunk_map(map);
out:
	btrfs_put_block_group(bg);
	return ret;
}

/*
 * Set up what a run of btrfs_scrub_raid56_full_stripe() calls needs, and hold
 * it across the whole run.
 *
 * A scrub_ctx carries SCRUB_TOTAL_STRIPES inline stripes with a full
 * BTRFS_STRIPE_LEN of pages each, so it is roughly 8 MiB; the data stripes of
 * the chunk come on top.  Allocating and freeing that for every full stripe
 * the log recorded makes a long recovery do nothing else, and every one of
 * those allocations is a chance to fail: wib_recover_one() turns -ENOMEM into
 * a failed mount, so a recovery of many stripes under memory pressure is far
 * more likely to fail than a recovery of one, for no reason inherent to the
 * work.  Allocated once here, the per-stripe cost is a reset.
 *
 * The workqueue reference is held for the same reason: without an outer one
 * the workqueue is created and destroyed again for every full stripe, and the
 * inner get can fail.  With it held the inner get is a refcount increment.
 *
 * Returns the context to pass to btrfs_scrub_raid56_full_stripe(), or an
 * ERR_PTR.
 */
struct scrub_ctx *btrfs_scrub_raid56_recovery_begin(struct btrfs_fs_info *fs_info)
{
	struct scrub_ctx *sctx;
	int ret;

	sctx = scrub_setup_ctx(fs_info, false);
	if (IS_ERR(sctx))
		return sctx;
	sctx->readonly = false;
	/*
	 * Deliberately not published in fs_info->scrubs_running.  This runs
	 * from the mount path, registers no device scrub_ctx, and cannot wait
	 * for a transaction; counting it would make btrfs_scrub_cancel() block
	 * uninterruptibly on a scrub it cannot reach, and would hold off a
	 * commit through the pause protocol for the whole recovery.
	 * Cancellation is handled by the signal check in should_cancel_scrub().
	 */
	sctx->internal = true;

	ret = scrub_workers_get(fs_info);
	if (ret < 0) {
		scrub_put_ctx(sctx);
		return ERR_PTR(ret);
	}
	return sctx;
}

void btrfs_scrub_raid56_recovery_end(struct btrfs_fs_info *fs_info,
				     struct scrub_ctx *sctx)
{
	scrub_workers_put(fs_info);
	if (!IS_ERR_OR_NULL(sctx))
		scrub_put_ctx(sctx);
}

/*
 * Testing only: recover a full stripe with a data column on a missing device
 * as this code did before it classified one -- the column rebuilt out of
 * whichever parity the read path reaches first, never compared with the
 * other, and the stripe never recorded as undecidable.  The negative control
 * for uml/degraded_crash.sh.
 */
#ifdef CONFIG_BTRFS_DEBUG
static bool recover_absent_legacy;
module_param_named(raid56_recover_absent_legacy, recover_absent_legacy, bool, 0644);
MODULE_PARM_DESC(raid56_recover_absent_legacy,
		 "Recover a full stripe whose data column is on a missing device as before: never compare its two parities, never record it as undecidable (testing only: restores a known defect)");
/*
 * Classify such a stripe only when its record names no member, as the first
 * version of scrub_raid56_recover_absent() did: one that names the missing
 * column -- a write into it failed, as every write into it does -- goes down
 * the ordinary path, which rebuilds the column out of the parity it is left
 * with and checks nothing.  The negative control for the "named" arms of
 * uml/degraded_crash.sh.
 */
static bool recover_absent_skip_named;
module_param_named(raid56_recover_absent_skip_named, recover_absent_skip_named, bool, 0644);
MODULE_PARM_DESC(raid56_recover_absent_skip_named,
		 "Recover a full stripe whose data column is on a missing device the old way whenever the write-intent log names a member of it (testing only: restores a known defect)");
/*
 * Record a RAID6 stripe undecidable when a checksummed or metadata sector of
 * its missing column matches neither rebuild, as the first version of
 * scrub_raid56_absent_pq() did, instead of leaving that sector unrepaired.
 */
static bool recover_absent_checked_suspect;
module_param_named(raid56_recover_absent_checked_suspect,
		   recover_absent_checked_suspect, bool, 0644);
MODULE_PARM_DESC(raid56_recover_absent_checked_suspect,
		 "Record a RAID6 full stripe undecidable when a checksummed sector of its missing data column matches neither parity's rebuild (testing only: restores the old behaviour)");
/*
 * Record a full stripe undecidable as scrub_raid56_mark_suspect() did first:
 * without an alert, whether the verdict was recorded or not.  The negative
 * control for the alert arm of uml/torn_present.sh.
 */
static bool recover_suspect_silent;
module_param_named(raid56_recover_suspect_silent, recover_suspect_silent, bool, 0644);
MODULE_PARM_DESC(raid56_recover_suspect_silent,
		 "Record a full stripe the recovery cannot decide without raising an alert (testing only: restores the old behaviour)");
#else
static const bool recover_absent_legacy;
static const bool recover_absent_skip_named;
static const bool recover_absent_checked_suspect;
static const bool recover_suspect_silent;
#endif

/*
 * Solve the absent data column @x of a full stripe twice, from what the disks
 * hold: out of P into @dp and out of Q into @dq.  The other data columns are
 * the ones the verify pass left in sctx->raid56_data_stripes; @x's own buffer
 * is not an input, and nothing is read from @x's device.
 *
 * Both parities are read straight off their devices (@pdev, at @pphys), a
 * page at a time.  Nothing else writes to the filesystem while the log is
 * being recovered, and the verify pass wrote no parity, so what is read is
 * what the crash left.
 *
 * Returns 0, -ENOMEM, or the error of a parity read.
 */
static int scrub_raid56_solve_absent(struct scrub_ctx *sctx, int data_stripes,
				     struct btrfs_device **pdev, const u64 *pphys,
				     int x, void *dp, void *dq)
{
	struct page *bounce = alloc_page(GFP_NOFS);
	struct page *scratch = alloc_page(GFP_NOFS);
	void **ptrs = kcalloc(data_stripes + 2, sizeof(*ptrs), GFP_NOFS);
	int ret = 0;

	if (!bounce || !scratch || !ptrs) {
		ret = -ENOMEM;
		goto out;
	}

	for (u32 off = 0; off < BTRFS_STRIPE_LEN; off += PAGE_SIZE) {
		int nr = 0;

		/* dP: P with every other data column taken out. */
		ret = bdev_rw_virt(pdev[0]->bdev, (pphys[0] + off) >> SECTOR_SHIFT,
				   page_address(bounce), PAGE_SIZE, REQ_OP_READ);
		if (ret)
			goto out;
		memcpy(dp + off, page_address(bounce), PAGE_SIZE);
		for (int i = 0; i < data_stripes; i++)
			if (i != x)
				ptrs[nr++] = sctx->raid56_data_stripes[i].buffer + off;
		if (nr)
			xor_gen(dp + off, ptrs, nr, PAGE_SIZE);

		/* dQ: the same, out of Q, with P recomputed on the side. */
		ret = bdev_rw_virt(pdev[1]->bdev, (pphys[1] + off) >> SECTOR_SHIFT,
				   page_address(bounce), PAGE_SIZE, REQ_OP_READ);
		if (ret)
			goto out;
		/* The Q of a lone data column is that column. */
		if (data_stripes == 1) {
			memcpy(dq + off, page_address(bounce), PAGE_SIZE);
			continue;
		}
		for (int i = 0; i < data_stripes; i++)
			ptrs[i] = i == x ? dq + off :
				  sctx->raid56_data_stripes[i].buffer + off;
		ptrs[data_stripes] = page_address(scratch);
		ptrs[data_stripes + 1] = page_address(bounce);
		raid6_recov_datap(data_stripes + 2, PAGE_SIZE, x, ptrs);
	}
out:
	kfree(ptrs);
	if (scratch)
		__free_page(scratch);
	if (bounce)
		__free_page(bounce);
	return ret;
}

/*
 * The full stripe at @full_stripe_start is suspect: a crash may have torn a
 * write into it while a data column it has to rebuild was on a missing device,
 * or was named stale by the record with no parity left over to check its
 * rebuild (SCRUB_WIB_TORN), and nothing can tell what that column held.
 * Record every parity that is still there as not describing the data, and
 * keep the record.
 *
 * That is all it takes for the rest of the system to refuse rather than guess,
 * because the column already counts as failed everywhere -- a missing
 * device's always, one the record names stale through mark_stale_sectors():
 * a read of one of its sectors without a checksum then needs more members
 * than the parity it may still use can rebuild, and fails with the
 * read_ambiguous alert (recover_rbio()); a checksummed sector is rebuilt and
 * verified as before, since mark_stale_sectors() exempts it; and a
 * read-modify-write into the stripe is refused as undecidable.  The record is
 * persisted with the log, so the next mount -- still degraded, or with the
 * device back, which is when the stripe can be decided again -- starts from
 * it: as the possibly torn stripe it is, from which that mount reaches this
 * verdict again, or regenerates the parity from the data once the device is
 * back.  The parity marks themselves are written only in a block that is wide
 * anyway (btrfs_wib_mark_suspect_parity()): made wide by them, the log holds
 * half as many regions, and a degraded recovery keeping more such stripes than
 * that spent the records of some of them to keep the others, their verdicts
 * with them.
 *
 * Whether or not the log could keep it, the administrator is told: the
 * stripe's unchecksummed data reads as EIO, or, if the verdict was lost, may
 * come back as a guess (BTRFS_RAID56_EV_TORN_UNDECIDABLE).
 *
 * Returns 4 (see btrfs_scrub_raid56_full_stripe()), or -EIO if the log could
 * not keep all of it.
 */
static int scrub_raid56_mark_suspect(struct btrfs_fs_info *fs_info,
				     u64 full_stripe_start, int data_stripes,
				     int nr_parity, unsigned int present_par)
{
	const u64 len = btrfs_stripe_nr_to_offset(data_stripes);
	struct btrfs_wib_stripe_state st;
	unsigned int marked = 0;

	btrfs_wib_add_sticky(fs_info, full_stripe_start, len);
	for (int p = 0; p < nr_parity; p++) {
		/*
		 * Parity p is recorded in block p of the full stripe (see
		 * btrfs_wib_disk_entry::stale_par), so a stripe with fewer data
		 * columns than parities cannot say it about its last parity.
		 */
		if (!(present_par & BIT(p)) || p >= data_stripes)
			continue;
		btrfs_wib_mark_suspect_parity(fs_info, full_stripe_start, len, p);
		marked |= BIT(p);
	}
	if (!READ_ONCE(recover_suspect_silent))
		btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_TORN_UNDECIDABLE,
				   full_stripe_start, NULL, 0);
	if (marked == present_par &&
	    btrfs_wib_stripe_state(fs_info, full_stripe_start, data_stripes,
				   nr_parity, &st) &&
	    (st.bad_parity & marked) == marked)
		return 4;
	btrfs_err(fs_info,
"scrub: full stripe %llu may hold a torn write that cannot be decided, but %s; a read of its data without a checksum that has to be rebuilt may return a wrong rebuild",
		  full_stripe_start, marked == present_par ?
		  "the write-intent log could not record it as undecidable" :
		  "the write-intent log cannot record the last parity of a stripe with fewer data columns than parities");
	return -EIO;
}

/*
 * RAID6, both parities present, one data column (@xs) missing: the vertical
 * stripe by vertical stripe classification that scrub_raid56_recover_absent()
 * describes, and the parity regeneration it decides.
 *
 * A checksummed or metadata sector of @xs that matches neither rebuild is not
 * a reason to call the stripe undecidable.  Its checksum already fails every
 * read of it, so a mark on the parities would protect nothing more there;
 * what the mark would do is fail the reads of the column's unchecksummed
 * sectors in the rows where dP == dQ proves the rebuild, which the read path's
 * own Q cross-check returns correctly, and refuse every write into the stripe
 * for as long as the device is gone.  So it is left unrepaired, as on RAID5,
 * and nothing is written: the parities are not regenerated from a column
 * holding a sector nothing could verify.  The stripe is undecidable only where
 * data without a checksum is at stake, i.e. dP != dQ in its rows.
 *
 * Returns 1, 4, -EIO or another negative error as that function does, or
 * -EAGAIN if a parity could not be read, so that nothing could be compared.
 */
static int scrub_raid56_absent_pq(struct scrub_ctx *sctx,
				  struct btrfs_chunk_map *map,
				  u64 full_stripe_start,
				  struct btrfs_device **pdev, const u64 *pphys,
				  struct scrub_stripe *xs,
				  unsigned long *extent_bitmap)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	const u32 sectorsize = fs_info->sectorsize;
	const int data_stripes = nr_data_stripes(map);
	const int nr_parity = map->num_stripes - data_stripes;
	const unsigned long has_extent = scrub_bitmap_read_has_extent(xs);
	const unsigned long is_metadata = scrub_bitmap_read_is_metadata(xs);
	const unsigned long error = scrub_bitmap_read_error(xs);
	unsigned long suspect = 0;
	unsigned long unrepaired = 0;
	void *dp = kvmalloc(BTRFS_STRIPE_LEN, GFP_NOFS);
	void *dq = kvmalloc(BTRFS_STRIPE_LEN, GFP_NOFS);
	int ret;

	if (!dp || !dq) {
		ret = -ENOMEM;
		goto out;
	}
	ret = scrub_raid56_solve_absent(sctx, data_stripes, pdev, pphys,
					xs - sctx->raid56_data_stripes, dp, dq);
	if (ret < 0 && ret != -ENOMEM) {
		btrfs_warn_rl(fs_info,
"scrub: full stripe %llu has a data column on missing devid %llu, and its parity could not be read to check it against: %d",
			      full_stripe_start, xs->dev->devid, ret);
		ret = -EAGAIN;
	}
	if (ret < 0)
		goto out;

	for (int nr = 0; nr < xs->nr_sectors; nr++) {
		const u32 off = nr << fs_info->sectorsize_bits;

		if (test_bit(nr, &has_extent) &&
		    (test_bit(nr, &is_metadata) || xs->sectors[nr].csum)) {
			/*
			 * The verify pass rebuilt it from P and then from Q
			 * and kept the first its checksum accepts.  If neither
			 * passed, nothing here can decide it either -- and its
			 * checksum refuses it at every read without help.
			 */
			if (!test_bit(nr, &error))
				continue;
			if (READ_ONCE(recover_absent_checked_suspect))
				__set_bit(nr, &suspect);
			else
				__set_bit(nr, &unrepaired);
			continue;
		}
		if (test_bit(nr, &has_extent) &&
		    memcmp(dp + off, dq + off, sectorsize) != 0) {
			__set_bit(nr, &suspect);
			continue;
		}
		/* Free, or both parities agree: the column is dP. */
		memcpy(xs->buffer + off, dp + off, sectorsize);
	}
	if (unrepaired)
		btrfs_warn_rl(fs_info,
"scrub: full stripe %llu: %u checksummed sector(s) of the data column on missing devid %llu match neither its rebuild from P nor the one from Q; they stay unrepaired and read as EIO, and the parity is left alone",
			      full_stripe_start,
			      bitmap_weight(&unrepaired, xs->nr_sectors),
			      xs->dev->devid);
	if (suspect) {
		btrfs_warn_rl(fs_info,
"scrub: full stripe %llu: its two parities disagree about %u sector(s) of the data column on missing devid %llu, and no checksum can say which is right",
			      full_stripe_start,
			      bitmap_weight(&suspect, xs->nr_sectors),
			      xs->dev->devid);
		ret = scrub_raid56_mark_suspect(fs_info, full_stripe_start,
						data_stripes, nr_parity, 3);
		goto out;
	}
	/*
	 * Nothing is written for a sector nothing verified: the regeneration
	 * below would fold whichever guess the verify pass left in the buffer
	 * into both parities, and with the device gone those are all there is.
	 */
	if (unrepaired) {
		ret = -EIO;
		goto out;
	}

	/*
	 * Decided.  Regenerate both parities from the data columns as they now
	 * stand in memory, the missing one included, without reading any of
	 * them again: Q (or, where the checksum chose dQ, P) is rewritten to
	 * agree.
	 */
	for (int p = 0; p < nr_parity; p++) {
		ret = scrub_raid56_cached_parity(sctx, pdev[p], map,
						 full_stripe_start, extent_bitmap);
		if (ret < 0)
			break;
	}
	/* A parity write failed (strict scrub rbio): still as it was. */
	if (ret == -EREMOTEIO)
		ret = -EIO;
	/*
	 * The column's device is still missing: the record stays, but the
	 * stripe is consistent -- its parities describe the data, the missing
	 * column's rows as decided -- and no longer possibly torn.
	 */
	if (ret == 0) {
		sctx->raid56_absent_decided = true;
		ret = 1;
	}
out:
	kvfree(dp);
	kvfree(dq);
	return ret;
}

/*
 * Write-intent log recovery of a full stripe with a data column on a device
 * that is not there: the crash happened while the array was degraded.
 *
 * The ordinary recovery rebuilds such a column like any unreadable one, from
 * the parity, and folds the result into the parity it regenerates.  After a
 * torn write that is a guess.  A write into another column that reached its
 * device while the parity did not (or the other way round) leaves the parity
 * describing a vector that never existed, and the missing column "rebuilt"
 * from it is the old content xor the change.  With a checksum the rebuild
 * fails verification and the read fails loudly; without one -- nodatacow,
 * nodatasum, preallocated extents -- the rebuild is simply returned, now and
 * at every later read, and on RAID6 it silently becomes the only answer the
 * moment a second device goes.  (syn_stripe_model.py: 384 to 161,280 wrong
 * reads for the old rule depending on geometry, 0 for the one below.)
 *
 * So classify the stripe instead, never reading the missing column (it cannot
 * be read anyway; its own checksum-verified content would be the one exception,
 * which matters only once a device can be present and distrusted at once).
 *
 * @unknown is every data column the parity has to stand in for: the ones on
 * a missing device, and the ones the record names stale, whose content on the
 * disk is not what was acknowledged.  @bad_parity is the parities the record
 * says no longer describe the data; with the missing ones they are not
 * "usable".  The caller only comes here when @unknown fits within the usable
 * parities: beyond that the record already fails every rebuild of those
 * columns' unchecksummed sectors (mark_stale_sectors()), a stripe marked
 * suspect by an earlier mount included, and there is nothing to decide.
 *
 * RAID6 with both parities usable and one unknown data column X, on the
 * missing device -- solve X out of P (dP) and out of Q (dQ), vertical stripe by
 * vertical stripe:
 *   dP == dQ			consistent; nothing to do.
 *   X holds no extent		X := dP; the regeneration rewrites Q to agree,
 *				so that a later loss of a second device finds
 *				two parities describing the same vector.
 *   X checksummed or metadata	the verify pass already took the first of
 *				dP, dQ that verifies, and the regeneration
 *				rewrites the parity that disagrees.  If neither
 *				verifies the sector stays unrepaired and nothing
 *				is written (scrub_raid56_absent_pq()).
 *   anything else		suspect.
 *
 * Otherwise -- RAID5, or RAID6 with a parity missing or recorded stale, or a
 * second data column missing or recorded stale -- there is nothing left to
 * compare with:
 *   an unknown column holds a referenced sector without a checksum
 *			suspect.
 *   otherwise		leave the parity alone.  A checksummed X was verified
 *			against the parity left, and regenerating that parity
 *			from the rebuild would give it back unchanged.
 *
 * One suspect vertical stripe makes the full stripe suspect, since the record
 * describes whole parities: scrub_raid56_mark_suspect().  Nothing is written
 * then, nor before it: the verify pass reads a column the record names as it
 * is, and rebuilds only what a checksum verifies (@raid56_verify_only), since
 * the rebuild the record would ask for -- from every parity, none left over --
 * is what the classification is deciding.  A present data column the verify
 * pass could not clean up counts as nothing to compare with too: dP and dQ
 * would be computed from it.
 *
 * The record naming X changes none of this, although it is the usual case: a
 * write into X while its device is gone "fails" there, so X is named stale
 * and the record kept -- and the write was acknowledged, its value only in the
 * parity.  A later write into the same full stripe, torn by the crash, then
 * leaves that parity describing a vector that never existed just as it does
 * for an X nobody named, and the name adds nothing a read could use: X is
 * failed already, so mark_stale_sectors() finds room in the budget, and the
 * single-parity rebuild runs unchecked.  Nor does the plan the ordinary path
 * makes from the record help (scrub_raid56_plan_wib()): a column on a missing
 * device is a hole it counts, never one it has to rebuild, and a stripe with
 * nothing else named passes it as having nothing to do.
 *
 * The cost is availability after any mount that meets such a record while
 * the device is still gone, crash or not: a record does not say whether a
 * write was torn after it was made, so a RAID5 stripe written into while
 * degraded refuses its missing column's unchecksummed data, and writes into
 * it, until the device is back.  Refused, not wrong.
 *
 * "Referenced" and "free" mean something only once the extent tree is
 * complete, so with a tree log still to replay (@classify false) this only
 * verifies and keeps the record; btrfs_wib_recover_after_replay() comes back
 * to it.
 *
 * Returns 1 (decided as far as it can be; the record stays while the device
 * is missing), 4 (suspect, recorded), -EIO if a present sector with an extent
 * or a checksummed sector of an unknown column could not be repaired, a
 * parity could not be read or written, or the suspect stripe could not be
 * recorded, or another negative error.
 */
static int scrub_raid56_recover_absent(struct scrub_ctx *sctx,
				       struct btrfs_block_group *bg,
				       struct btrfs_chunk_map *map,
				       u64 full_stripe_start, int rot,
				       u64 unknown, unsigned int bad_parity,
				       bool classify)
{
	struct btrfs_fs_info *fs_info = sctx->fs_info;
	const int data_stripes = nr_data_stripes(map);
	const int nr_parity = map->num_stripes - data_stripes;
	struct btrfs_device *pdev[2] = { NULL, NULL };
	u64 pphys[2] = { 0, 0 };
	unsigned long extent_bitmap = 0;
	unsigned int present_par = 0;
	unsigned int usable_par;
	bool present_unclean = false;
	bool unrepaired = false;
	struct scrub_stripe *xs = NULL;
	u64 nocsum_devid = 0;
	bool nocsum_missing = false;
	int ret;

	for (int p = 0; p < nr_parity; p++) {
		const int idx = (data_stripes + p + rot) % map->num_stripes;

		if (!map->stripes[idx].dev->bdev)
			continue;
		pdev[p] = map->stripes[idx].dev;
		pphys[p] = map->stripes[idx].physical + btrfs_stripe_nr_to_offset(rot);
		present_par |= BIT(p);
	}
	usable_par = present_par & ~bad_parity;
	/* No parity left: nothing to verify against or to record anything on. */
	if (!present_par)
		return 1;

	/*
	 * Verify and repair the data columns, and write no parity.  Nor act on
	 * the record: the columns it names are among @unknown, which is what
	 * is being decided (scrub_raid56_plan_wib()).
	 */
	sctx->raid56_verify_only = true;
	ret = scrub_raid56_parity_stripe(sctx, pdev[__ffs(present_par)], bg, map,
					 full_stripe_start, false);
	sctx->raid56_verify_only = false;
	if (ret < 0)
		return ret;

	for (int i = 0; i < data_stripes; i++) {
		struct scrub_stripe *stripe = &sctx->raid56_data_stripes[i];
		const unsigned long has_extent = scrub_bitmap_read_has_extent(stripe);
		const unsigned long is_metadata = scrub_bitmap_read_is_metadata(stripe);
		const unsigned long error = scrub_bitmap_read_error(stripe);
		int nr;

		/* The block group went away before the pass read anything. */
		if (stripe->logical != full_stripe_start + btrfs_stripe_nr_to_offset(i))
			return 1;
		extent_bitmap |= has_extent;
		if (!(unknown & BIT_ULL(i))) {
			/*
			 * Any error, a free sector's too: dP and dQ would be
			 * computed from whatever that buffer holds.
			 */
			if (error || stripe->write_error_bitmap)
				present_unclean = true;
			if ((error | stripe->write_error_bitmap) & has_extent)
				unrepaired = true;
			continue;
		}
		/*
		 * A missing column's write errors are the repair writes to a
		 * device that is not there, which always fail: not news.  A
		 * named column on a device that is there took its repair or
		 * did not.
		 */
		if (stripe->dev->bdev && (stripe->write_error_bitmap & has_extent))
			unrepaired = true;
		for_each_set_bit(nr, &has_extent, stripe->nr_sectors) {
			if (!test_bit(nr, &is_metadata) && !stripe->sectors[nr].csum) {
				nocsum_devid = stripe->dev->devid;
				nocsum_missing = !stripe->dev->bdev;
			} else if (test_bit(nr, &error)) {
				unrepaired = true;
			}
		}
		xs = stripe;
	}
	/* Nothing referenced anywhere in the full stripe: nothing to lose. */
	if (!extent_bitmap)
		return 1;
	if (!classify)
		return unrepaired ? -EIO : 1;

	/*
	 * One unknown column is the missing one (the caller found one), and
	 * @xs is it.
	 */
	if (nr_parity == 2 && usable_par == 3 && hweight64(unknown) == 1 &&
	    !present_unclean &&
	    btrfs_wib_stripe_error(fs_info, full_stripe_start, data_stripes) !=
	    BTRFS_WIB_STRIPE_PARTIAL_ERROR) {
		ret = scrub_raid56_absent_pq(sctx, map, full_stripe_start, pdev,
					     pphys, xs, &extent_bitmap);
		if (ret != -EAGAIN)
			return ret;
		/* A parity did not read: nothing to compare with after all. */
		unrepaired = true;
	}
	if (!nocsum_devid)
		return unrepaired ? -EIO : 1;
	btrfs_warn_rl(fs_info,
"scrub: full stripe %llu: devid %llu holds data without a checksum in a data column that has to be rebuilt (%s), and no parity is left over to check that rebuild against",
		      full_stripe_start, nocsum_devid,
		      nocsum_missing ? "its device is missing" :
		      "the write-intent log records it stale");
	return scrub_raid56_mark_suspect(fs_info, full_stripe_start, data_stripes,
					 nr_parity, present_par);
}

/*
 * Scrub one RAID56 full stripe for the write-intent log recovery: verify
 * every sector that holds an extent (data checksums, tree block headers),
 * repair the bad ones from the parity and write them back, and recompute
 * the parity of the vertical stripes that hold extents from the verified
 * data.
 *
 * @mode: see enum btrfs_raid56_recover_mode.  VERIFY writes no parity, since
 * the scrub would recompute it from sectors it cannot verify.  SCRUB decides
 * from the write-intent record, exactly as a user scrub does.  TRUSTED means
 * the sectors that hold no extent are known to be what was last written to
 * them (see btrfs_wib_recover()): if the verification passed and no device is
 * missing, the parity of every vertical stripe is additionally recomputed from
 * the data on disk, which covers extents the extent tree does not know yet
 * (tree log).
 *
 * A full stripe with a data column on a missing device is classified by
 * scrub_raid56_recover_absent() instead, if @torn -- its record says a write
 * into it may have been torn, see wib_recover_one() -- except in VERIFY mode
 * and unless its record already makes it undecidable.
 * @log_replay_pending: a tree log is still to be replayed, so the extent tree
 * does not know every extent yet; such a stripe is then only verified and kept
 * for btrfs_wib_recover_after_replay().
 * @unwritten_par: set, on a return of 1, to the parities on a missing device
 * when they are all that is missing and every parity that is there was
 * regenerated from the data: the stripe is consistent but for those.  Or to
 * BTRFS_WIB_STRIPE_DECIDED when a data column is what is missing, and the
 * classification decided it and regenerated both parities
 * (scrub_raid56_absent_pq()).  0 otherwise.
 *
 * Return 0 if every extent found was verified or repaired (and, unless
 * VERIFY, the full stripe is consistent), 1 if a device of the chunk is
 * missing (its sectors were not repaired), 3 in SCRUB mode if the scrub
 * declined the stripe or could not get its repair onto the disk (the record
 * must stay), 2 if every extent was verified
 * but the parity of a vertical stripe with an unreadable sector holding no
 * extent could not be recomputed, 4 if a torn write left the stripe
 * undecidable -- a data column on a missing device, or one the record names
 * that no parity left over could check the rebuild of (every present parity is
 * recorded stale; the record must stay) -- -EIO if a sector holding an extent
 * could not be repaired or a parity write failed (the parity is then left
 * alone where it may still allow the repair later), -ENOENT if
 * @full_stripe_start is not in a RAID56 block group (anymore), or another
 * negative error.
 */
int btrfs_scrub_raid56_full_stripe(struct btrfs_fs_info *fs_info,
				   struct scrub_ctx *sctx,
				   u64 full_stripe_start,
				   enum btrfs_raid56_recover_mode mode,
				   bool log_replay_pending, bool torn,
				   unsigned int *unwritten_par)
{
	const bool regen = mode != BTRFS_RAID56_RECOVER_VERIFY;
	struct btrfs_wib_stripe_state st;
	struct btrfs_block_group *bg;
	struct btrfs_chunk_map *map = NULL;
	u64 fstripe_len;
	bool missing = false;
	u64 absent = 0;
	int data_stripes;
	u32 rem;
	int rot;
	int ret;

	*unwritten_par = 0;
	bg = btrfs_lookup_block_group(fs_info, full_stripe_start);
	if (!bg)
		return -ENOENT;
	if (!(bg->flags & BTRFS_BLOCK_GROUP_RAID56_MASK)) {
		ret = -ENOENT;
		goto out_bg;
	}
	map = btrfs_find_chunk_map(fs_info, bg->start, bg->length);
	if (!map) {
		ret = -ENOENT;
		goto out_bg;
	}
	data_stripes = nr_data_stripes(map);
	fstripe_len = btrfs_stripe_nr_to_offset(data_stripes);
	/* The full stripe length is not a power of two, no IS_ALIGNED() here. */
	rot = div_u64_rem(full_stripe_start - bg->start, fstripe_len, &rem);
	ASSERT(rem == 0);
	for (int i = 0; i < map->num_stripes; i++) {
		if (!map->stripes[i].dev->bdev)
			missing = true;
	}
	/* The data columns on a missing device, in full stripe order. */
	if (missing && data_stripes <= 64 &&
	    map->num_stripes - data_stripes <= 2) {
		for (int c = 0; c < data_stripes; c++)
			if (!map->stripes[(c + rot) % map->num_stripes].dev->bdev)
				absent |= BIT_ULL(c);
	}

	/* Reused across the run; only a differently shaped chunk reallocates. */
	ret = scrub_alloc_raid56_data_stripes(sctx, bg, data_stripes);
	if (ret < 0)
		goto out_map;

	sctx->raid56_defer_retire = mode == BTRFS_RAID56_RECOVER_SCRUB;
	sctx->raid56_keep_record = false;
	sctx->raid56_torn = torn;
	sctx->raid56_torn_undecided = false;
	sctx->raid56_absent_decided = false;

	/*
	 * A data column on a missing device.  The passes below would rebuild it
	 * from whichever parity the read path reaches first and fold that into
	 * the parity they regenerate, which after a torn write is a guess; see
	 * scrub_raid56_recover_absent().
	 *
	 * Whatever the record names.  The usual record here names the missing
	 * column itself -- every write into it while the device is gone fails
	 * there -- and the plan the passes would make from it
	 * (scrub_raid56_plan_wib()) never has to rebuild a column on a device
	 * that is not there, so it has nothing to do, and the passes rebuild
	 * the column exactly as for a stripe nothing named.  The named columns
	 * are unknown along with the missing ones, and the parities the record
	 * condemns cannot be used to decide them.
	 *
	 * The one stripe left to the passes is one with more unknown columns
	 * than usable parities: the record already fails every rebuild of their
	 * unchecksummed sectors, and there is nothing to compare with.  That is
	 * the verdict of an earlier mount's pass through here too -- every
	 * parity still there recorded stale -- which stands until the device
	 * is back.
	 *
	 * And only where a write into the stripe may have been torn (@torn): in
	 * flight at the crash, or marked possibly torn before it.  A record of
	 * a plain failed write names what the failure left stale and nothing
	 * else went wrong since, so the parity describes the acknowledged data
	 * and the passes rebuild the missing column from it exactly.
	 * Classified, its unchecksummed sectors would read as EIO until the
	 * device is back, for nothing.
	 */
	if (absent && torn && mode != BTRFS_RAID56_RECOVER_VERIFY &&
	    !READ_ONCE(recover_absent_legacy)) {
		const int nr_parity = map->num_stripes - data_stripes;
		const bool named = btrfs_wib_stripe_state(fs_info, full_stripe_start,
							  data_stripes, nr_parity,
							  &st);
		const u64 unknown = absent | st.stale_cols;
		unsigned int usable = 0;

		for (int p = 0; p < nr_parity; p++) {
			const int idx = (data_stripes + p + rot) % map->num_stripes;

			if (map->stripes[idx].dev->bdev && !(st.bad_parity & BIT(p)))
				usable |= BIT(p);
		}
		if (!(named && READ_ONCE(recover_absent_skip_named)) &&
		    hweight64(unknown) <= hweight32(usable)) {
			ret = scrub_raid56_recover_absent(sctx, bg, map,
							  full_stripe_start, rot,
							  unknown, st.bad_parity,
							  !log_replay_pending);
			if (ret == 1 && sctx->raid56_absent_decided)
				*unwritten_par = BTRFS_WIB_STRIPE_DECIDED;
			goto out_map;
		}
		/*
		 * The parities the record condemns stand -- as the verdict of
		 * an earlier mount, reloaded from a wide log block as the stale
		 * parities it made, or as parities a write left stale -- and
		 * they are all that keeps a read of the missing column's
		 * unchecksummed data from a rebuild out of them: a full log
		 * spends them last, as it does this mount's verdicts.
		 */
		if (st.bad_parity)
			btrfs_wib_keep_prior_verdict(fs_info, full_stripe_start,
						     st.bad_parity);
	}

	/*
	 * Every parity stripe of the full stripe is regenerated by a separate
	 * pass; each pass verifies and repairs the data stripes, which is
	 * cheap compared to the corruption it prevents.
	 */
	for (int p = data_stripes; p < map->num_stripes; p++) {
		struct btrfs_device *pdev = map->stripes[(p + rot) % map->num_stripes].dev;

		/* A missing parity device gets its parity rebuilt when replaced. */
		if (!pdev->bdev)
			continue;
		ret = scrub_raid56_parity_stripe(sctx, pdev, bg, map, full_stripe_start,
						 regen);
		if (ret < 0)
			break;
		ret = 0;
	}
	/*
	 * scrub_raid56_parity_stripe() does not report unrepaired sectors
	 * through its return value (and the stripes carry NO_REPORT), look
	 * at the stripes of the last pass directly.
	 */
	if (ret == -EREMOTEIO) {
		/* A parity write failed (strict scrub rbio): still stale. */
		ret = -EIO;
	}
	/*
	 * A pass left the stripe untouched because a write into it may have
	 * been torn and no parity was left over to check the rebuild of a
	 * column the record names (SCRUB_WIB_TORN).  Undecidable, as a stripe
	 * with such a column on a missing device is: record it so, so that the
	 * reads and writes that would need that rebuild fail rather than make
	 * it (scrub_raid56_mark_suspect()).  Whatever else went wrong in the
	 * stripe, this is its verdict.
	 */
	if (ret == 0 && sctx->raid56_torn_undecided) {
		const int nr_parity = map->num_stripes - data_stripes;
		unsigned int present_par = 0;

		for (int p = 0; p < nr_parity; p++)
			if (map->stripes[(data_stripes + p + rot) % map->num_stripes].dev->bdev)
				present_par |= BIT(p);
		ret = present_par ?
		      scrub_raid56_mark_suspect(fs_info, full_stripe_start, data_stripes,
						nr_parity, present_par) : 3;
		goto out_map;
	}
	if (ret == 0) {
		for (int i = 0; i < data_stripes; i++) {
			struct scrub_stripe *stripe = &sctx->raid56_data_stripes[i];
			unsigned long error = scrub_bitmap_read_error(stripe);
			unsigned long has_extent = scrub_bitmap_read_has_extent(stripe);

			/* Repair writes completed before REPAIR_DONE was set. */
			bitmap_or(&error, &error, &stripe->write_error_bitmap,
				  stripe->nr_sectors);
			bitmap_and(&error, &error, &has_extent, stripe->nr_sectors);
			if (!bitmap_empty(&error, stripe->nr_sectors)) {
				ret = -EIO;
				break;
			}
		}
	}
	/*
	 * Consistent but for the parities on a missing device: every data
	 * column is there and was verified, every parity that is there was
	 * regenerated from it, and no pass declined anything -- nor had to
	 * regenerate without the extents only a tree log still to be replayed
	 * knows.  The missing parities were not rewritten; what they hold is
	 * all that may still describe a write a crash tore, and the caller
	 * records them stale rather than the stripe possibly torn.
	 */
	if (ret == 0 && missing && regen && !log_replay_pending &&
	    !sctx->raid56_keep_record) {
		const int nr_parity = map->num_stripes - data_stripes;
		unsigned int gone = 0;
		bool data_gone = false;

		for (int c = 0; c < map->num_stripes; c++) {
			if (map->stripes[(c + rot) % map->num_stripes].dev->bdev)
				continue;
			if (c < data_stripes)
				data_gone = true;
			else
				gone |= BIT(c - data_stripes);
		}
		if (!data_gone && gone != GENMASK(nr_parity - 1, 0))
			*unwritten_par = gone;
	}
	if (ret == 0 && missing)
		ret = 1;
	/*
	 * The scrub's own verdict, for the one mode that asked for it: some
	 * pass declined the stripe, found it unrepairable, or could not get a
	 * repair onto the disk.  Keep the record; the caller says why.
	 */
	if (ret == 0 && mode == BTRFS_RAID56_RECOVER_SCRUB && sctx->raid56_keep_record)
		ret = 3;
	sctx->raid56_defer_retire = false;
	if (ret == 0 && mode == BTRFS_RAID56_RECOVER_TRUSTED) {
		for (int p = data_stripes; p < map->num_stripes; p++) {
			struct btrfs_device *pdev =
				map->stripes[(p + rot) % map->num_stripes].dev;

			ret = scrub_raid56_full_parity(sctx, pdev, map, full_stripe_start);
			if (ret < 0)
				break;
		}
		if (ret == -EREMOTEIO) {
			ret = -EIO;
		} else if (ret < 0) {
			/*
			 * Every extent was verified above; what could not be
			 * read holds none.
			 */
			ret = 2;
		}
	}

out_map:
	sctx->raid56_defer_retire = false;
	sctx->raid56_torn = false;
	btrfs_free_chunk_map(map);
out_bg:
	btrfs_put_block_group(bg);
	return ret;
}

int btrfs_scrub_dev(struct btrfs_fs_info *fs_info, u64 devid, u64 start,
		    u64 end, struct btrfs_scrub_progress *progress,
		    bool readonly, bool is_dev_replace)
{
	struct btrfs_dev_lookup_args args = { .devid = devid };
	struct scrub_ctx *sctx;
	int ret;
	struct btrfs_device *dev;
	unsigned int nofs_flag;
	bool need_commit = false;

	/* Set the basic fallback @last_physical before we got a sctx. */
	if (progress)
		progress->last_physical = start;

	if (btrfs_fs_closing(fs_info))
		return -EAGAIN;

	/* At mount time we have ensured nodesize is in the range of [4K, 64K]. */
	ASSERT(fs_info->nodesize <= BTRFS_STRIPE_LEN);

	/* Allocate outside of device_list_mutex */
	sctx = scrub_setup_ctx(fs_info, is_dev_replace);
	if (IS_ERR(sctx))
		return PTR_ERR(sctx);
	sctx->stat.last_physical = start;

	ret = scrub_workers_get(fs_info);
	if (ret)
		goto out_free_ctx;

	mutex_lock(&fs_info->fs_devices->device_list_mutex);
	dev = btrfs_find_device(fs_info->fs_devices, &args);
	if (!dev || (test_bit(BTRFS_DEV_STATE_MISSING, &dev->dev_state) &&
		     !is_dev_replace)) {
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		ret = -ENODEV;
		goto out;
	}

	if (!is_dev_replace && !readonly &&
	    !test_bit(BTRFS_DEV_STATE_WRITEABLE, &dev->dev_state)) {
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		btrfs_err(fs_info,
			"scrub: devid %llu: filesystem on %s is not writable",
				 devid, btrfs_dev_name(dev));
		ret = -EROFS;
		goto out;
	}

	mutex_lock(&fs_info->scrub_lock);
	if (unlikely(!test_bit(BTRFS_DEV_STATE_IN_FS_METADATA, &dev->dev_state) ||
		     test_bit(BTRFS_DEV_STATE_REPLACE_TGT, &dev->dev_state))) {
		mutex_unlock(&fs_info->scrub_lock);
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		ret = -EIO;
		goto out;
	}

	down_read(&fs_info->dev_replace.rwsem);
	if (dev->scrub_ctx ||
	    (!is_dev_replace &&
	     btrfs_dev_replace_is_ongoing(&fs_info->dev_replace))) {
		up_read(&fs_info->dev_replace.rwsem);
		mutex_unlock(&fs_info->scrub_lock);
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);
		ret = -EINPROGRESS;
		goto out;
	}
	up_read(&fs_info->dev_replace.rwsem);

	sctx->readonly = readonly;
	dev->scrub_ctx = sctx;
	mutex_unlock(&fs_info->fs_devices->device_list_mutex);

	/*
	 * checking @scrub_pause_req here, we can avoid
	 * race between committing transaction and scrubbing.
	 */
	__scrub_blocked_if_needed(fs_info);
	atomic_inc(&fs_info->scrubs_running);
	mutex_unlock(&fs_info->scrub_lock);

	/*
	 * In order to avoid deadlock with reclaim when there is a transaction
	 * trying to pause scrub, make sure we use GFP_NOFS for all the
	 * allocations done at btrfs_scrub_sectors() and scrub_sectors_for_parity()
	 * invoked by our callees. The pausing request is done when the
	 * transaction commit starts, and it blocks the transaction until scrub
	 * is paused (done at specific points at scrub_stripe() or right above
	 * before incrementing fs_info->scrubs_running).
	 */
	nofs_flag = memalloc_nofs_save();
	if (!is_dev_replace) {
		u64 old_super_errors;

		spin_lock(&sctx->stat_lock);
		old_super_errors = sctx->stat.super_errors;
		spin_unlock(&sctx->stat_lock);

		btrfs_info(fs_info, "scrub: started on devid %llu", devid);
		/*
		 * by holding device list mutex, we can
		 * kick off writing super in log tree sync.
		 */
		mutex_lock(&fs_info->fs_devices->device_list_mutex);
		ret = scrub_supers(sctx, dev);
		mutex_unlock(&fs_info->fs_devices->device_list_mutex);

		spin_lock(&sctx->stat_lock);
		/*
		 * Super block errors found, but we can not commit transaction
		 * at current context, since btrfs_commit_transaction() needs
		 * to pause the current running scrub (hold by ourselves).
		 */
		if (sctx->stat.super_errors > old_super_errors && !sctx->readonly)
			need_commit = true;
		spin_unlock(&sctx->stat_lock);
	}

	if (!ret)
		ret = scrub_enumerate_chunks(sctx, dev, start, end);
	memalloc_nofs_restore(nofs_flag);

	atomic_dec(&fs_info->scrubs_running);
	wake_up(&fs_info->scrub_pause_wait);

	if (progress)
		memcpy(progress, &sctx->stat, sizeof(*progress));

	if (!is_dev_replace)
		btrfs_info(fs_info, "scrub: %s on devid %llu with status: %d",
			ret ? "not finished" : "finished", devid, ret);

	mutex_lock(&fs_info->scrub_lock);
	dev->scrub_ctx = NULL;
	mutex_unlock(&fs_info->scrub_lock);

	scrub_workers_put(fs_info);
	scrub_put_ctx(sctx);

	/*
	 * We found some super block errors before, now try to force a
	 * transaction commit, as scrub has finished.
	 */
	if (need_commit) {
		struct btrfs_trans_handle *trans;

		trans = btrfs_start_transaction(fs_info->tree_root, 0);
		if (IS_ERR(trans)) {
			ret = PTR_ERR(trans);
			btrfs_err(fs_info,
	"scrub: failed to start transaction to fix super block errors: %d", ret);
			return ret;
		}
		ret = btrfs_commit_transaction(trans);
		if (ret < 0)
			btrfs_err(fs_info,
	"scrub: failed to commit transaction to fix super block errors: %d", ret);
	}
	return ret;
out:
	scrub_workers_put(fs_info);
out_free_ctx:
	scrub_free_ctx(sctx);

	return ret;
}

void btrfs_scrub_pause(struct btrfs_fs_info *fs_info)
{
	mutex_lock(&fs_info->scrub_lock);
	atomic_inc(&fs_info->scrub_pause_req);
	while (atomic_read(&fs_info->scrubs_paused) !=
	       atomic_read(&fs_info->scrubs_running)) {
		mutex_unlock(&fs_info->scrub_lock);
		wait_event(fs_info->scrub_pause_wait,
			   atomic_read(&fs_info->scrubs_paused) ==
			   atomic_read(&fs_info->scrubs_running));
		mutex_lock(&fs_info->scrub_lock);
	}
	mutex_unlock(&fs_info->scrub_lock);
}

void btrfs_scrub_continue(struct btrfs_fs_info *fs_info)
{
	atomic_dec(&fs_info->scrub_pause_req);
	wake_up(&fs_info->scrub_pause_wait);
}

int btrfs_scrub_cancel(struct btrfs_fs_info *fs_info)
{
	mutex_lock(&fs_info->scrub_lock);
	if (!atomic_read(&fs_info->scrubs_running)) {
		mutex_unlock(&fs_info->scrub_lock);
		return -ENOTCONN;
	}

	atomic_inc(&fs_info->scrub_cancel_req);
	while (atomic_read(&fs_info->scrubs_running)) {
		mutex_unlock(&fs_info->scrub_lock);
		wait_event(fs_info->scrub_pause_wait,
			   atomic_read(&fs_info->scrubs_running) == 0);
		mutex_lock(&fs_info->scrub_lock);
	}
	atomic_dec(&fs_info->scrub_cancel_req);
	mutex_unlock(&fs_info->scrub_lock);

	return 0;
}

int btrfs_scrub_cancel_dev(struct btrfs_device *dev)
{
	struct btrfs_fs_info *fs_info = dev->fs_info;
	struct scrub_ctx *sctx;

	mutex_lock(&fs_info->scrub_lock);
	sctx = dev->scrub_ctx;
	if (!sctx) {
		mutex_unlock(&fs_info->scrub_lock);
		return -ENOTCONN;
	}
	atomic_inc(&sctx->cancel_req);
	while (dev->scrub_ctx) {
		mutex_unlock(&fs_info->scrub_lock);
		wait_event(fs_info->scrub_pause_wait,
			   dev->scrub_ctx == NULL);
		mutex_lock(&fs_info->scrub_lock);
	}
	mutex_unlock(&fs_info->scrub_lock);

	return 0;
}

int btrfs_scrub_progress(struct btrfs_fs_info *fs_info, u64 devid,
			 struct btrfs_scrub_progress *progress)
{
	struct btrfs_dev_lookup_args args = { .devid = devid };
	struct btrfs_device *dev;
	struct scrub_ctx *sctx = NULL;

	mutex_lock(&fs_info->fs_devices->device_list_mutex);
	dev = btrfs_find_device(fs_info->fs_devices, &args);
	if (dev)
		sctx = dev->scrub_ctx;
	if (sctx)
		memcpy(progress, &sctx->stat, sizeof(*progress));
	mutex_unlock(&fs_info->fs_devices->device_list_mutex);

	return dev ? (sctx ? 0 : -ENOTCONN) : -ENODEV;
}
