// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Fusion-io  All rights reserved.
 * Copyright (C) 2012 Intel Corp. All rights reserved.
 */

#include <linux/sched.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/raid/pq.h>
#include <linux/hash.h>
#include <linux/list_sort.h>
#include <linux/raid/xor.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include "messages.h"
#include "ctree.h"
#include "disk-io.h"
#include "volumes.h"
#include "raid56.h"
#include "async-thread.h"
#include "file-item.h"
#include "btrfs_inode.h"
#include "raid56-wib.h"
#include "ordered-data.h"
#include "dev-replace.h"
#include "block-group.h"

/* set when additional merges to this rbio are not allowed */
#define RBIO_RMW_LOCKED_BIT	1

/*
 * set when this rbio is sitting in the hash, but it is just a cache
 * of past RMW
 */
#define RBIO_CACHE_BIT		2

/*
 * set when it is safe to trust the stripe_pages for caching
 */
#define RBIO_CACHE_READY_BIT	3

/*
 * Set if any bio of the rbio overwrites referenced sectors in place
 * (nodatacow or preallocated extents).  Such a write must be recorded in
 * the write-intent log even when it covers the full stripe: a crash in the
 * middle of it leaves committed data without valid parity.
 */
#define RBIO_INPLACE_BIT	4

/*
 * Set on a parity scrub rbio used by the write-intent log recovery: a
 * failed parity write fails the rbio even when it is within the RAID's
 * tolerance, because the recovery must know that the parity on disk is
 * still stale.
 */
#define RBIO_STRICT_WRITE_BIT	5

/*
 * Set by mark_stale_sectors() when the write-intent log names more members of
 * the full stripe than its surviving parity can rebuild.  A read can do no
 * better than return what is on disk; a read-modify-write must not write at
 * all, see rmw_rbio().
 */
#define RBIO_STALE_AMBIGUOUS_BIT	6

/*
 * A repair rbio: a read-modify-write with no data of its own, submitted by
 * btrfs_raid56_repair_work() for a full stripe a write left damaged.  It
 * reads the stripe, rebuilds what the record proves stale and writes that and
 * the parity back -- exactly the repair any later write to the stripe would
 * do, done without waiting for one.  Never merged with another rbio, and it
 * never asks for a repair of its own; a failure is retried by the queue.
 */
#define RBIO_REPAIR_BIT		7

#ifdef CONFIG_BTRFS_DEBUG
/*
 * Crash injection for testing the write-intent log, module parameter
 * btrfs.raid56_crash_point:
 *
 *  1: the next sub-stripe RMW drops its P/Q writes, waits for its data
 *     writes to complete and then panics.
 *  2: the next sub-stripe RMW drops its data writes, waits for its P/Q
 *     writes to complete and then panics.
 *  3: like 1 but without the panic, leaving a silently stale parity behind.
 *  4: the next parity scrub (as used by the write-intent log replay) panics
 *     before writing the regenerated parity (can be given on the kernel
 *     command line to crash the replay).
 *
 * Only the first matching rbio after the parameter is set is affected.
 */
static int btrfs_raid56_crash_point;
module_param_named(raid56_crash_point, btrfs_raid56_crash_point, int, 0644);
MODULE_PARM_DESC(raid56_crash_point,
		 "Inject a crash into the next RAID56 sub-stripe write (testing only)");
#endif

#define RBIO_CACHE_SIZE 1024

#define BTRFS_STRIPE_HASH_TABLE_BITS				11

static void dump_bioc(const struct btrfs_fs_info *fs_info, const struct btrfs_io_context *bioc)
{
	if (unlikely(!bioc)) {
		btrfs_crit(fs_info, "bioc=NULL");
		return;
	}
	btrfs_crit(fs_info,
"bioc logical=%llu full_stripe=%llu size=%llu map_type=0x%llx mirror=%u replace_nr_stripes=%u replace_stripe_src=%d num_stripes=%u",
		bioc->logical, bioc->full_stripe_logical, bioc->size,
		bioc->map_type, bioc->mirror_num, bioc->replace_nr_stripes,
		bioc->replace_stripe_src, bioc->num_stripes);
	for (int i = 0; i < bioc->num_stripes; i++) {
		btrfs_crit(fs_info, "    nr=%d devid=%llu physical=%llu",
			   i, bioc->stripes[i].dev->devid,
			   bioc->stripes[i].physical);
	}
}

static void btrfs_dump_rbio(const struct btrfs_fs_info *fs_info,
			    const struct btrfs_raid_bio *rbio)
{
	if (!IS_ENABLED(CONFIG_BTRFS_ASSERT))
		return;

	dump_bioc(fs_info, rbio->bioc);
	btrfs_crit(fs_info,
"rbio flags=0x%lx nr_sectors=%u nr_data=%u real_stripes=%u stripe_nsectors=%u sector_nsteps=%u scrubp=%u dbitmap=0x%lx",
		rbio->flags, rbio->nr_sectors, rbio->nr_data,
		rbio->real_stripes, rbio->stripe_nsectors,
		rbio->sector_nsteps, rbio->scrubp, rbio->dbitmap);
}

#define ASSERT_RBIO(expr, rbio)						\
({									\
	if (IS_ENABLED(CONFIG_BTRFS_ASSERT) && unlikely(!(expr))) {	\
		const struct btrfs_fs_info *__fs_info = (rbio)->bioc ?	\
					(rbio)->bioc->fs_info : NULL;	\
									\
		btrfs_dump_rbio(__fs_info, (rbio));			\
	}								\
	ASSERT((expr));							\
})

#define ASSERT_RBIO_STRIPE(expr, rbio, stripe_nr)			\
({									\
	if (IS_ENABLED(CONFIG_BTRFS_ASSERT) && unlikely(!(expr))) {	\
		const struct btrfs_fs_info *__fs_info = (rbio)->bioc ?	\
					(rbio)->bioc->fs_info : NULL;	\
									\
		btrfs_dump_rbio(__fs_info, (rbio));			\
		btrfs_crit(__fs_info, "stripe_nr=%d", (stripe_nr));	\
	}								\
	ASSERT((expr));							\
})

#define ASSERT_RBIO_SECTOR(expr, rbio, sector_nr)			\
({									\
	if (IS_ENABLED(CONFIG_BTRFS_ASSERT) && unlikely(!(expr))) {	\
		const struct btrfs_fs_info *__fs_info = (rbio)->bioc ?	\
					(rbio)->bioc->fs_info : NULL;	\
									\
		btrfs_dump_rbio(__fs_info, (rbio));			\
		btrfs_crit(__fs_info, "sector_nr=%d", (sector_nr));	\
	}								\
	ASSERT((expr));							\
})

#define ASSERT_RBIO_LOGICAL(expr, rbio, logical)			\
({									\
	if (IS_ENABLED(CONFIG_BTRFS_ASSERT) && unlikely(!(expr))) {	\
		const struct btrfs_fs_info *__fs_info = (rbio)->bioc ?	\
					(rbio)->bioc->fs_info : NULL;	\
									\
		btrfs_dump_rbio(__fs_info, (rbio));			\
		btrfs_crit(__fs_info, "logical=%llu", (logical));		\
	}								\
	ASSERT((expr));							\
})

/* Used by the raid56 code to lock stripes for read/modify/write */
struct btrfs_stripe_hash {
	struct list_head hash_list;
	spinlock_t lock;
};

/* Used by the raid56 code to lock stripes for read/modify/write */
struct btrfs_stripe_hash_table {
	struct list_head stripe_cache;
	spinlock_t cache_lock;
	int cache_size;
	struct btrfs_stripe_hash table[];
};

/*
 * The PFN may still be valid, but our paddrs should always be block size
 * aligned, thus such -1 paddr is definitely not a valid one.
 */
#define INVALID_PADDR	(~(phys_addr_t)0)

static void rmw_rbio_work(struct work_struct *work);
static void rmw_rbio_work_locked(struct work_struct *work);
static void index_rbio_pages(struct btrfs_raid_bio *rbio);
static int alloc_rbio_pages(struct btrfs_raid_bio *rbio);

static int finish_parity_scrub(struct btrfs_raid_bio *rbio);
static void fill_data_csums(struct btrfs_raid_bio *rbio);
static phys_addr_t *sector_paddrs_in_rbio(struct btrfs_raid_bio *rbio,
					  int stripe_nr, int sector_nr,
					  bool bio_list_only);
static phys_addr_t sector_paddr_in_rbio(struct btrfs_raid_bio *rbio, int stripe_nr,
					int sector_nr, int step_nr, bool bio_list_only);
void *kmap_local_paddr(phys_addr_t paddr);
static void scrub_rbio_work_locked(struct work_struct *work);

static void free_raid_bio_pointers(struct btrfs_raid_bio *rbio)
{
	bitmap_free(rbio->error_bitmap);
	bitmap_free(rbio->stripe_uptodate_bitmap);
	bitmap_free(rbio->verified_bitmap);
	bitmap_free(rbio->repair_bitmap);
	kfree(rbio->stripe_pages);
	kfree(rbio->bio_paddrs);
	kfree(rbio->stripe_paddrs);
	kfree(rbio->finish_pointers);
}

static void free_raid_bio(struct btrfs_raid_bio *rbio)
{
	int i;

	if (!refcount_dec_and_test(&rbio->refs))
		return;

	WARN_ON(!list_empty(&rbio->stripe_cache));
	WARN_ON(!list_empty(&rbio->hash_list));
	WARN_ON(!bio_list_empty(&rbio->bio_list));

	for (i = 0; i < rbio->nr_pages; i++) {
		if (rbio->stripe_pages[i]) {
			__free_page(rbio->stripe_pages[i]);
			rbio->stripe_pages[i] = NULL;
		}
	}

	btrfs_put_bioc(rbio->bioc);
	free_raid_bio_pointers(rbio);
	kfree(rbio);
}

static void start_async_work(struct btrfs_raid_bio *rbio, work_func_t work_func)
{
	INIT_WORK(&rbio->work, work_func);
	queue_work(rbio->bioc->fs_info->rmw_workers, &rbio->work);
}

/*
 * the stripe hash table is used for locking, and to collect
 * bios in hopes of making a full stripe
 */
int btrfs_alloc_stripe_hash_table(struct btrfs_fs_info *info)
{
	struct btrfs_stripe_hash_table *table;
	struct btrfs_stripe_hash_table *x;
	struct btrfs_stripe_hash *cur;
	struct btrfs_stripe_hash *h;
	unsigned int num_entries = 1U << BTRFS_STRIPE_HASH_TABLE_BITS;

	if (info->stripe_hash_table)
		return 0;

	/*
	 * The table is large, starting with order 4 and can go as high as
	 * order 7 in case lock debugging is turned on.
	 *
	 * Try harder to allocate and fallback to vmalloc to lower the chance
	 * of a failing mount.
	 */
	table = kvzalloc_flex(*table, table, num_entries);
	if (!table)
		return -ENOMEM;

	spin_lock_init(&table->cache_lock);
	INIT_LIST_HEAD(&table->stripe_cache);

	h = table->table;

	for (unsigned int i = 0; i < num_entries; i++) {
		cur = h + i;
		INIT_LIST_HEAD(&cur->hash_list);
		spin_lock_init(&cur->lock);
	}

	x = cmpxchg(&info->stripe_hash_table, NULL, table);
	kvfree(x);
	return 0;
}

static void memcpy_from_bio_to_stripe(struct btrfs_raid_bio *rbio, unsigned int sector_nr)
{
	const u32 step = min(rbio->bioc->fs_info->sectorsize, PAGE_SIZE);

	ASSERT(sector_nr < rbio->nr_sectors);
	for (int i = 0; i < rbio->sector_nsteps; i++) {
		unsigned int index = sector_nr * rbio->sector_nsteps + i;
		phys_addr_t dst = rbio->stripe_paddrs[index];
		phys_addr_t src = rbio->bio_paddrs[index];

		ASSERT(dst != INVALID_PADDR);
		ASSERT(src != INVALID_PADDR);

		memcpy_page(phys_to_page(dst), offset_in_page(dst),
			    phys_to_page(src), offset_in_page(src), step);
	}
}

/*
 * caching an rbio means to copy anything from the
 * bio_sectors array into the stripe_pages array.  We
 * use the page uptodate bit in the stripe cache array
 * to indicate if it has valid data
 *
 * once the caching is done, we set the cache ready
 * bit.
 */
static void cache_rbio_pages(struct btrfs_raid_bio *rbio)
{
	int i;
	int ret;

	ret = alloc_rbio_pages(rbio);
	if (ret)
		return;

	for (i = 0; i < rbio->nr_sectors; i++) {
		/* Some range not covered by bio (partial write), skip it */
		if (rbio->bio_paddrs[i * rbio->sector_nsteps] == INVALID_PADDR) {
			/*
			 * Even if the sector is not covered by bio, if it is
			 * a data sector it should still be uptodate as it is
			 * read from disk.
			 */
			if (i < rbio->nr_data * rbio->stripe_nsectors)
				ASSERT(test_bit(i, rbio->stripe_uptodate_bitmap));
			continue;
		}

		memcpy_from_bio_to_stripe(rbio, i);
		set_bit(i, rbio->stripe_uptodate_bitmap);
	}
	set_bit(RBIO_CACHE_READY_BIT, &rbio->flags);
}

/*
 * we hash on the first logical address of the stripe
 */
static int rbio_bucket(struct btrfs_raid_bio *rbio)
{
	u64 num = rbio->bioc->full_stripe_logical;

	/*
	 * we shift down quite a bit.  We're using byte
	 * addressing, and most of the lower bits are zeros.
	 * This tends to upset hash_64, and it consistently
	 * returns just one or two different values.
	 *
	 * shifting off the lower bits fixes things.
	 */
	return hash_64(num >> 16, BTRFS_STRIPE_HASH_TABLE_BITS);
}

/* Get the sector number of the first sector covered by @page_nr. */
static u32 page_nr_to_sector_nr(struct btrfs_raid_bio *rbio, unsigned int page_nr)
{
	u32 sector_nr;

	ASSERT(page_nr < rbio->nr_pages);

	sector_nr = (page_nr << PAGE_SHIFT) >> rbio->bioc->fs_info->sectorsize_bits;
	ASSERT(sector_nr < rbio->nr_sectors);
	return sector_nr;
}

/*
 * Get the number of sectors covered by @page_nr.
 *
 * For bs > ps cases, the result will always be 1.
 * For bs <= ps cases, the result will be ps / bs.
 */
static u32 page_nr_to_num_sectors(struct btrfs_raid_bio *rbio, unsigned int page_nr)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	u32 nr_sectors;

	ASSERT(page_nr < rbio->nr_pages);

	nr_sectors = round_up(PAGE_SIZE, fs_info->sectorsize) >> fs_info->sectorsize_bits;
	ASSERT(nr_sectors > 0);
	return nr_sectors;
}

static __maybe_unused bool full_page_sectors_uptodate(struct btrfs_raid_bio *rbio,
						      unsigned int page_nr)
{
	const u32 sector_nr = page_nr_to_sector_nr(rbio, page_nr);
	const u32 nr_bits = page_nr_to_num_sectors(rbio, page_nr);
	int i;

	ASSERT(page_nr < rbio->nr_pages);
	ASSERT(sector_nr + nr_bits < rbio->nr_sectors);

	for (i = sector_nr; i < sector_nr + nr_bits; i++) {
		if (!test_bit(i, rbio->stripe_uptodate_bitmap))
			return false;
	}
	return true;
}

/*
 * Update the stripe_sectors[] array to use correct page and pgoff
 *
 * Should be called every time any page pointer in stripes_pages[] got modified.
 */
static void index_stripe_sectors(struct btrfs_raid_bio *rbio)
{
	const u32 step = min(rbio->bioc->fs_info->sectorsize, PAGE_SIZE);
	u32 offset;
	int i;

	for (i = 0, offset = 0; i < rbio->nr_sectors * rbio->sector_nsteps;
	     i++, offset += step) {
		int page_index = offset >> PAGE_SHIFT;

		ASSERT(page_index < rbio->nr_pages);
		if (!rbio->stripe_pages[page_index])
			continue;

		rbio->stripe_paddrs[i] = page_to_phys(rbio->stripe_pages[page_index]) +
					 offset_in_page(offset);
	}
}

static void steal_rbio_page(struct btrfs_raid_bio *src,
			    struct btrfs_raid_bio *dest, int page_nr)
{
	const u32 sector_nr = page_nr_to_sector_nr(src, page_nr);
	const u32 nr_bits = page_nr_to_num_sectors(src, page_nr);

	ASSERT(page_nr < src->nr_pages);
	ASSERT(sector_nr + nr_bits < src->nr_sectors);

	if (dest->stripe_pages[page_nr])
		__free_page(dest->stripe_pages[page_nr]);
	dest->stripe_pages[page_nr] = src->stripe_pages[page_nr];
	src->stripe_pages[page_nr] = NULL;

	/* Also update the stripe_uptodate_bitmap bits. */
	bitmap_set(dest->stripe_uptodate_bitmap, sector_nr, nr_bits);
}

static bool is_data_stripe_page(struct btrfs_raid_bio *rbio, int page_nr)
{
	const int sector_nr = page_nr_to_sector_nr(rbio, page_nr);

	/*
	 * We have ensured PAGE_SIZE is aligned with sectorsize, thus
	 * we won't have a page which is half data half parity.
	 *
	 * Thus if the first sector of the page belongs to data stripes, then
	 * the full page belongs to data stripes.
	 */
	return (sector_nr < rbio->nr_data * rbio->stripe_nsectors);
}

/*
 * Stealing an rbio means taking all the uptodate pages from the stripe array
 * in the source rbio and putting them into the destination rbio.
 *
 * This will also update the involved stripe_sectors[] which are referring to
 * the old pages.
 */
static void steal_rbio(struct btrfs_raid_bio *src, struct btrfs_raid_bio *dest)
{
	int i;

	if (!test_bit(RBIO_CACHE_READY_BIT, &src->flags))
		return;

	for (i = 0; i < dest->nr_pages; i++) {
		struct page *p = src->stripe_pages[i];

		/*
		 * We don't need to steal P/Q pages as they will always be
		 * regenerated for RMW or full write anyway.
		 */
		if (!is_data_stripe_page(src, i))
			continue;

		/*
		 * If @src already has RBIO_CACHE_READY_BIT, it should have
		 * all data stripe pages present and uptodate.
		 */
		ASSERT(p);
		ASSERT(full_page_sectors_uptodate(src, i));
		steal_rbio_page(src, dest, i);
	}
	index_stripe_sectors(dest);
	index_stripe_sectors(src);
}

/*
 * merging means we take the bio_list from the victim and
 * splice it into the destination.  The victim should
 * be discarded afterwards.
 *
 * must be called with dest->rbio_list_lock held
 */
static void merge_rbio(struct btrfs_raid_bio *dest,
		       struct btrfs_raid_bio *victim)
{
	bio_list_merge_init(&dest->bio_list, &victim->bio_list);
	dest->bio_list_bytes += victim->bio_list_bytes;
	/* Also inherit the bitmaps from @victim. */
	bitmap_or(&dest->dbitmap, &victim->dbitmap, &dest->dbitmap,
		  dest->stripe_nsectors);
	if (test_bit(RBIO_INPLACE_BIT, &victim->flags))
		set_bit(RBIO_INPLACE_BIT, &dest->flags);
}

/*
 * used to prune items that are in the cache.  The caller
 * must hold the hash table lock.
 */
static void __remove_rbio_from_cache(struct btrfs_raid_bio *rbio)
{
	int bucket = rbio_bucket(rbio);
	struct btrfs_stripe_hash_table *table;
	struct btrfs_stripe_hash *h;
	bool freeit = false;

	/*
	 * check the bit again under the hash table lock.
	 */
	if (!test_bit(RBIO_CACHE_BIT, &rbio->flags))
		return;

	table = rbio->bioc->fs_info->stripe_hash_table;
	h = table->table + bucket;

	/* hold the lock for the bucket because we may be
	 * removing it from the hash table
	 */
	spin_lock(&h->lock);

	/*
	 * hold the lock for the bio list because we need
	 * to make sure the bio list is empty
	 */
	spin_lock(&rbio->bio_list_lock);

	if (test_and_clear_bit(RBIO_CACHE_BIT, &rbio->flags)) {
		list_del_init(&rbio->stripe_cache);
		table->cache_size -= 1;
		freeit = true;

		/* if the bio list isn't empty, this rbio is
		 * still involved in an IO.  We take it out
		 * of the cache list, and drop the ref that
		 * was held for the list.
		 *
		 * If the bio_list was empty, we also remove
		 * the rbio from the hash_table, and drop
		 * the corresponding ref
		 */
		if (bio_list_empty(&rbio->bio_list)) {
			if (!list_empty(&rbio->hash_list)) {
				list_del_init(&rbio->hash_list);
				refcount_dec(&rbio->refs);
				BUG_ON(!list_empty(&rbio->plug_list));
			}
		}
	}

	spin_unlock(&rbio->bio_list_lock);
	spin_unlock(&h->lock);

	if (freeit)
		free_raid_bio(rbio);
}

/*
 * prune a given rbio from the cache
 */
static void remove_rbio_from_cache(struct btrfs_raid_bio *rbio)
{
	struct btrfs_stripe_hash_table *table;

	if (!test_bit(RBIO_CACHE_BIT, &rbio->flags))
		return;

	table = rbio->bioc->fs_info->stripe_hash_table;

	spin_lock(&table->cache_lock);
	__remove_rbio_from_cache(rbio);
	spin_unlock(&table->cache_lock);
}

/*
 * remove everything in the cache
 */
static void btrfs_clear_rbio_cache(struct btrfs_fs_info *info)
{
	struct btrfs_stripe_hash_table *table;
	struct btrfs_raid_bio *rbio;

	table = info->stripe_hash_table;

	spin_lock(&table->cache_lock);
	while (!list_empty(&table->stripe_cache)) {
		rbio = list_first_entry(&table->stripe_cache,
					struct btrfs_raid_bio, stripe_cache);
		__remove_rbio_from_cache(rbio);
	}
	spin_unlock(&table->cache_lock);
}

/*
 * remove all cached entries and free the hash table
 * used by unmount
 */
void btrfs_free_stripe_hash_table(struct btrfs_fs_info *info)
{
	if (!info->stripe_hash_table)
		return;
	btrfs_clear_rbio_cache(info);
	kvfree(info->stripe_hash_table);
	info->stripe_hash_table = NULL;
}

/*
 * insert an rbio into the stripe cache.  It
 * must have already been prepared by calling
 * cache_rbio_pages
 *
 * If this rbio was already cached, it gets
 * moved to the front of the lru.
 *
 * If the size of the rbio cache is too big, we
 * prune an item.
 */
static void cache_rbio(struct btrfs_raid_bio *rbio)
{
	struct btrfs_stripe_hash_table *table;

	if (!test_bit(RBIO_CACHE_READY_BIT, &rbio->flags))
		return;

	table = rbio->bioc->fs_info->stripe_hash_table;

	spin_lock(&table->cache_lock);
	spin_lock(&rbio->bio_list_lock);

	/* bump our ref if we were not in the list before */
	if (!test_and_set_bit(RBIO_CACHE_BIT, &rbio->flags))
		refcount_inc(&rbio->refs);

	if (!list_empty(&rbio->stripe_cache)){
		list_move(&rbio->stripe_cache, &table->stripe_cache);
	} else {
		list_add(&rbio->stripe_cache, &table->stripe_cache);
		table->cache_size += 1;
	}

	spin_unlock(&rbio->bio_list_lock);

	if (table->cache_size > RBIO_CACHE_SIZE) {
		struct btrfs_raid_bio *found;

		found = list_last_entry(&table->stripe_cache,
					struct btrfs_raid_bio,
					stripe_cache);

		if (found != rbio)
			__remove_rbio_from_cache(found);
	}

	spin_unlock(&table->cache_lock);
}

/*
 * Returns true if the bio list inside this rbio covers an entire stripe (no
 * rmw required).
 */
static int rbio_is_full(struct btrfs_raid_bio *rbio)
{
	unsigned long size = rbio->bio_list_bytes;
	int ret = 1;

	spin_lock(&rbio->bio_list_lock);
	if (size != rbio->nr_data * BTRFS_STRIPE_LEN)
		ret = 0;
	BUG_ON(size > rbio->nr_data * BTRFS_STRIPE_LEN);
	spin_unlock(&rbio->bio_list_lock);

	return ret;
}

/*
 * returns 1 if it is safe to merge two rbios together.
 * The merging is safe if the two rbios correspond to
 * the same stripe and if they are both going in the same
 * direction (read vs write), and if neither one is
 * locked for final IO
 *
 * The caller is responsible for locking such that
 * rmw_locked is safe to test
 */
static int rbio_can_merge(struct btrfs_raid_bio *last,
			  struct btrfs_raid_bio *cur)
{
	if (test_bit(RBIO_RMW_LOCKED_BIT, &last->flags) ||
	    test_bit(RBIO_RMW_LOCKED_BIT, &cur->flags))
		return 0;

	/*
	 * we can't merge with cached rbios, since the
	 * idea is that when we merge the destination
	 * rbio is going to run our IO for us.  We can
	 * steal from cached rbios though, other functions
	 * handle that.
	 */
	if (test_bit(RBIO_CACHE_BIT, &last->flags) ||
	    test_bit(RBIO_CACHE_BIT, &cur->flags))
		return 0;

	if (last->bioc->full_stripe_logical != cur->bioc->full_stripe_logical)
		return 0;

	/* we can't merge with different operations */
	if (last->operation != cur->operation)
		return 0;
	/*
	 * We've need read the full stripe from the drive.
	 * check and repair the parity and write the new results.
	 *
	 * We're not allowed to add any new bios to the
	 * bio list here, anyone else that wants to
	 * change this stripe needs to do their own rmw.
	 */
	if (last->operation == BTRFS_RBIO_PARITY_SCRUB)
		return 0;

	if (last->operation == BTRFS_RBIO_READ_REBUILD)
		return 0;

	/*
	 * A repair rbio carries its own completion accounting (the queue's
	 * in-flight count and a bio counter); merging would free it without.
	 */
	if (test_bit(RBIO_REPAIR_BIT, &last->flags) ||
	    test_bit(RBIO_REPAIR_BIT, &cur->flags))
		return 0;

	return 1;
}

/* Return the sector index for @stripe_nr and @sector_nr. */
static unsigned int rbio_sector_index(const struct btrfs_raid_bio *rbio,
				      unsigned int stripe_nr,
				      unsigned int sector_nr)
{
	unsigned int ret;

	ASSERT_RBIO_STRIPE(stripe_nr < rbio->real_stripes, rbio, stripe_nr);
	ASSERT_RBIO_SECTOR(sector_nr < rbio->stripe_nsectors, rbio, sector_nr);

	ret = stripe_nr * rbio->stripe_nsectors + sector_nr;
	ASSERT(ret < rbio->nr_sectors);
	return ret;
}

/* Return the paddr array index for @stripe_nr, @sector_nr and @step_nr. */
static unsigned int rbio_paddr_index(const struct btrfs_raid_bio *rbio,
				     unsigned int stripe_nr,
				     unsigned int sector_nr,
				     unsigned int step_nr)
{
	unsigned int ret;

	ASSERT_RBIO_SECTOR(step_nr < rbio->sector_nsteps, rbio, step_nr);

	ret = rbio_sector_index(rbio, stripe_nr, sector_nr) * rbio->sector_nsteps + step_nr;
	ASSERT(ret < rbio->nr_sectors * rbio->sector_nsteps);
	return ret;
}

static phys_addr_t rbio_stripe_paddr(const struct btrfs_raid_bio *rbio,
					  unsigned int stripe_nr, unsigned int sector_nr,
					  unsigned int step_nr)
{
	return rbio->stripe_paddrs[rbio_paddr_index(rbio, stripe_nr, sector_nr, step_nr)];
}

static phys_addr_t rbio_pstripe_paddr(const struct btrfs_raid_bio *rbio,
					   unsigned int sector_nr, unsigned int step_nr)
{
	return rbio_stripe_paddr(rbio, rbio->nr_data, sector_nr, step_nr);
}

static phys_addr_t rbio_qstripe_paddr(const struct btrfs_raid_bio *rbio,
					   unsigned int sector_nr, unsigned int step_nr)
{
	if (rbio->nr_data + 1 == rbio->real_stripes)
		return INVALID_PADDR;
	return rbio_stripe_paddr(rbio, rbio->nr_data + 1, sector_nr, step_nr);
}

/* Return a paddr pointer into the rbio::stripe_paddrs[] for the specified sector. */
static phys_addr_t *rbio_stripe_paddrs(const struct btrfs_raid_bio *rbio,
				       unsigned int stripe_nr, unsigned int sector_nr)
{
	return &rbio->stripe_paddrs[rbio_paddr_index(rbio, stripe_nr, sector_nr, 0)];
}

/*
 * The first stripe in the table for a logical address
 * has the lock.  rbios are added in one of three ways:
 *
 * 1) Nobody has the stripe locked yet.  The rbio is given
 * the lock and 0 is returned.  The caller must start the IO
 * themselves.
 *
 * 2) Someone has the stripe locked, but we're able to merge
 * with the lock owner.  The rbio is freed and the IO will
 * start automatically along with the existing rbio.  1 is returned.
 *
 * 3) Someone has the stripe locked, but we're not able to merge.
 * The rbio is added to the lock owner's plug list, or merged into
 * an rbio already on the plug list.  When the lock owner unlocks,
 * the next rbio on the list is run and the IO is started automatically.
 * 1 is returned
 *
 * If we return 0, the caller still owns the rbio and must continue with
 * IO submission.  If we return 1, the caller must assume the rbio has
 * already been freed.
 */
static noinline int lock_stripe_add(struct btrfs_raid_bio *rbio)
{
	struct btrfs_stripe_hash *h;
	struct btrfs_raid_bio *cur;
	struct btrfs_raid_bio *pending;
	struct btrfs_raid_bio *freeit = NULL;
	struct btrfs_raid_bio *cache_drop = NULL;
	int ret = 0;

	h = rbio->bioc->fs_info->stripe_hash_table->table + rbio_bucket(rbio);

	spin_lock(&h->lock);
	list_for_each_entry(cur, &h->hash_list, hash_list) {
		if (cur->bioc->full_stripe_logical != rbio->bioc->full_stripe_logical)
			continue;

		spin_lock(&cur->bio_list_lock);

		/* Can we steal this cached rbio's pages? */
		if (bio_list_empty(&cur->bio_list) &&
		    list_empty(&cur->plug_list) &&
		    test_bit(RBIO_CACHE_BIT, &cur->flags) &&
		    !test_bit(RBIO_RMW_LOCKED_BIT, &cur->flags)) {
			list_del_init(&cur->hash_list);
			refcount_dec(&cur->refs);

			steal_rbio(cur, rbio);
			cache_drop = cur;
			spin_unlock(&cur->bio_list_lock);

			goto lockit;
		}

		/* Can we merge into the lock owner? */
		if (rbio_can_merge(cur, rbio)) {
			merge_rbio(cur, rbio);
			spin_unlock(&cur->bio_list_lock);
			freeit = rbio;
			ret = 1;
			goto out;
		}


		/*
		 * We couldn't merge with the running rbio, see if we can merge
		 * with the pending ones.  We don't have to check for rmw_locked
		 * because there is no way they are inside finish_rmw right now
		 */
		list_for_each_entry(pending, &cur->plug_list, plug_list) {
			if (rbio_can_merge(pending, rbio)) {
				merge_rbio(pending, rbio);
				spin_unlock(&cur->bio_list_lock);
				freeit = rbio;
				ret = 1;
				goto out;
			}
		}

		/*
		 * No merging, put us on the tail of the plug list, our rbio
		 * will be started with the currently running rbio unlocks
		 */
		list_add_tail(&rbio->plug_list, &cur->plug_list);
		spin_unlock(&cur->bio_list_lock);
		ret = 1;
		goto out;
	}
lockit:
	refcount_inc(&rbio->refs);
	list_add(&rbio->hash_list, &h->hash_list);
out:
	spin_unlock(&h->lock);
	if (cache_drop)
		remove_rbio_from_cache(cache_drop);
	if (freeit)
		free_raid_bio(freeit);
	return ret;
}

static void recover_rbio_work_locked(struct work_struct *work);

/*
 * called as rmw or parity rebuild is completed.  If the plug list has more
 * rbios waiting for this stripe, the next one on the list will be started
 */
static noinline void unlock_stripe(struct btrfs_raid_bio *rbio)
{
	int bucket;
	struct btrfs_stripe_hash *h;
	bool keep_cache = false;

	bucket = rbio_bucket(rbio);
	h = rbio->bioc->fs_info->stripe_hash_table->table + bucket;

	if (list_empty(&rbio->plug_list))
		cache_rbio(rbio);

	spin_lock(&h->lock);
	spin_lock(&rbio->bio_list_lock);

	if (!list_empty(&rbio->hash_list)) {
		/*
		 * if we're still cached and there is no other IO
		 * to perform, just leave this rbio here for others
		 * to steal from later
		 */
		if (list_empty(&rbio->plug_list) &&
		    test_bit(RBIO_CACHE_BIT, &rbio->flags)) {
			keep_cache = true;
			clear_bit(RBIO_RMW_LOCKED_BIT, &rbio->flags);
			BUG_ON(!bio_list_empty(&rbio->bio_list));
			goto done;
		}

		list_del_init(&rbio->hash_list);
		refcount_dec(&rbio->refs);

		/*
		 * we use the plug list to hold all the rbios
		 * waiting for the chance to lock this stripe.
		 * hand the lock over to one of them.
		 */
		if (!list_empty(&rbio->plug_list)) {
			struct btrfs_raid_bio *next;
			struct list_head *head = rbio->plug_list.next;

			next = list_entry(head, struct btrfs_raid_bio,
					  plug_list);

			list_del_init(&rbio->plug_list);

			list_add(&next->hash_list, &h->hash_list);
			refcount_inc(&next->refs);
			spin_unlock(&rbio->bio_list_lock);
			spin_unlock(&h->lock);

			if (next->operation == BTRFS_RBIO_READ_REBUILD) {
				start_async_work(next, recover_rbio_work_locked);
			} else if (next->operation == BTRFS_RBIO_WRITE) {
				steal_rbio(rbio, next);
				start_async_work(next, rmw_rbio_work_locked);
			} else if (next->operation == BTRFS_RBIO_PARITY_SCRUB) {
				steal_rbio(rbio, next);
				start_async_work(next, scrub_rbio_work_locked);
			}

			goto done_nolock;
		}
	}
done:
	spin_unlock(&rbio->bio_list_lock);
	spin_unlock(&h->lock);

done_nolock:
	if (!keep_cache)
		remove_rbio_from_cache(rbio);
}

static void rbio_endio_bio_list(struct bio *cur, blk_status_t status)
{
	struct bio *next;

	while (cur) {
		next = cur->bi_next;
		cur->bi_next = NULL;
		cur->bi_status = status;
		bio_endio(cur);
		cur = next;
	}
}

/*
 * this frees the rbio and runs through all the bios in the
 * bio_list and calls end_io on them
 */
/*
 * Did anything check every data sector this read is about to hand back?
 *
 * A degraded read delivers a mix: sectors read directly from a present device,
 * verified by verify_bio_data_sectors(), and sectors rebuilt from the parity,
 * verified by verify_one_sector().  Both set a bit in @verified_bitmap.  A
 * sector that is in the caller's bio, has a checksum available, and carries no
 * such bit reached the caller through NEITHER -- and that is the only way left
 * for a checksummed read to return content that nothing rejected.
 *
 * Diagnostic only: it reports, it does not change what is returned.  Silence
 * here is the claim that the read path has no unverified delivery; a non-zero
 * count names the case that was missing.
 */
static void audit_delivered_sectors(struct btrfs_raid_bio *rbio, blk_status_t status)
{
	unsigned int unchecked = 0;
	unsigned int nocsum = 0;
	unsigned int zeros = 0;

	if (rbio->operation != BTRFS_RBIO_READ_REBUILD || status != BLK_STS_OK)
		return;
	if (!rbio->csum_bitmap || !rbio->csum_buf || !rbio->verified_bitmap) {
		/*
		 * Cannot audit this one: without the bitmap there is no way to
		 * say which sectors had a checksum available.  Count it, or a
		 * zero above would be indistinguishable from never looking --
		 * and this is exactly the case where nothing was verified.
		 */
		for (int stripe_nr = 0; stripe_nr < rbio->nr_data; stripe_nr++) {
			for (int s = 0; s < rbio->stripe_nsectors; s++) {
				if (sector_paddrs_in_rbio(rbio, stripe_nr, s, true)) {
					atomic64_inc(&rbio->bioc->fs_info->
						     raid56_write_stats.delivered_audit_skipped);
					return;
				}
			}
		}
		return;
	}

	for (int stripe_nr = 0; stripe_nr < rbio->nr_data; stripe_nr++) {
		for (int sector_nr = 0; sector_nr < rbio->stripe_nsectors; sector_nr++) {
			const int idx = stripe_nr * rbio->stripe_nsectors + sector_nr;

			if (!sector_paddrs_in_rbio(rbio, stripe_nr, sector_nr, true))
				continue;
			if (!test_bit(idx, rbio->csum_bitmap)) {
				/*
				 * Delivered, and no checksum covers it.  Both
				 * verify paths skip these, so nothing checked
				 * it and nothing could -- count it separately
				 * rather than let it hide behind a zero.
				 */
				nocsum++;
				continue;
			}
			if (!test_bit(rbio_sector_index(rbio, stripe_nr, sector_nr),
				      rbio->verified_bitmap))
				unchecked++;
		}
	}
	/*
	 * Is any sector being handed back entirely zero?
	 *
	 * An affected file has exactly one 4KiB sector of zeros in the middle
	 * of otherwise correct content, on a range that filefrag says is
	 * covered by a single extent with no hole and that GET_CSUMS says is
	 * checksummed.  So this is not a hole being read.  If the zeros are
	 * already here, they came out of the rbio -- a rebuild whose inputs
	 * were all zero, or a sector nothing ever populated.  If they are NOT
	 * here, the rbio handed back good bytes and something above zeroed
	 * them, which is a different search.
	 *
	 * Observation, not a guess: it says which half of the read path to
	 * look in, and costs one scan of the sectors already being audited.
	 */
	for (int stripe_nr = 0; stripe_nr < rbio->nr_data; stripe_nr++) {
		for (int sector_nr = 0; sector_nr < rbio->stripe_nsectors; sector_nr++) {
			const u32 step = min(rbio->bioc->fs_info->sectorsize, PAGE_SIZE);
			bool all_zero = true;

			if (!sector_paddrs_in_rbio(rbio, stripe_nr, sector_nr, true))
				continue;
			for (int i = 0; i < rbio->sector_nsteps && all_zero; i++) {
				phys_addr_t paddr =
					sector_paddr_in_rbio(rbio, stripe_nr, sector_nr, i, 0);
				void *p = kmap_local_paddr(paddr);

				if (memchr_inv(p, 0, step))
					all_zero = false;
				kunmap_local(p);
			}
			if (all_zero)
				zeros++;
			if (unlikely(btrfs_raid56_trace_reads())) {
				const int idx = stripe_nr * rbio->stripe_nsectors + sector_nr;

				btrfs_info(rbio->bioc->fs_info,
"raid56: RTRACE deliver logical %llu csum_bit %d verified %d zero %d",
					   rbio->bioc->full_stripe_logical +
					   ((u64)stripe_nr << BTRFS_STRIPE_LEN_SHIFT) +
					   ((u64)sector_nr << rbio->bioc->fs_info->sectorsize_bits),
					   test_bit(idx, rbio->csum_bitmap) ? 1 : 0,
					   test_bit(rbio_sector_index(rbio, stripe_nr, sector_nr),
						    rbio->verified_bitmap) ? 1 : 0,
					   all_zero ? 1 : 0);
			}
		}
	}
	if (unlikely(zeros)) {
		atomic64_add(zeros,
			     &rbio->bioc->fs_info->raid56_write_stats.delivered_zero);
		btrfs_warn_rl(rbio->bioc->fs_info,
"raid56: DELIVERED_ZERO %u sector(s) of full stripe %llu handed back entirely zero",
			      zeros, rbio->bioc->full_stripe_logical);
	}
	if (unlikely(nocsum))
		atomic64_add(nocsum,
			     &rbio->bioc->fs_info->raid56_write_stats.delivered_nocsum);
	if (unlikely(unchecked)) {
		atomic64_add(unchecked,
			     &rbio->bioc->fs_info->raid56_write_stats.delivered_unchecked);
		btrfs_warn_rl(rbio->bioc->fs_info,
"raid56: DELIVERED_UNCHECKED %u sector(s) of full stripe %llu returned to the caller with a checksum available that nothing compared them against",
			      unchecked, rbio->bioc->full_stripe_logical);
	}
}

static void rbio_orig_end_io(struct btrfs_raid_bio *rbio, blk_status_t status)
{
	struct bio *cur = bio_list_get(&rbio->bio_list);
	struct bio *extra;

	audit_delivered_sectors(rbio, status);
	kfree(rbio->csum_buf);
	bitmap_free(rbio->csum_bitmap);
	rbio->csum_buf = NULL;
	rbio->csum_bitmap = NULL;

	/*
	 * Clear the data bitmap, as the rbio may be cached for later usage.
	 * do this before before unlock_stripe() so there will be no new bio
	 * for this bio.
	 */
	bitmap_clear(&rbio->dbitmap, 0, rbio->stripe_nsectors);

	/*
	 * At this moment, rbio->bio_list is empty, however since rbio does not
	 * always have RBIO_RMW_LOCKED_BIT set and rbio is still linked on the
	 * hash list, rbio may be merged with others so that rbio->bio_list
	 * becomes non-empty.
	 * Once unlock_stripe() is done, rbio->bio_list will not be updated any
	 * more and we can call bio_endio() on all queued bios.
	 */
	unlock_stripe(rbio);
	extra = bio_list_get(&rbio->bio_list);
	free_raid_bio(rbio);

	rbio_endio_bio_list(cur, status);
	if (extra)
		rbio_endio_bio_list(extra, status);
}

/*
 * Get paddr pointer for the sector specified by its @stripe_nr and @sector_nr.
 *
 * @rbio:               The raid bio
 * @stripe_nr:          Stripe number, valid range [0, real_stripe)
 * @sector_nr:		Sector number inside the stripe,
 *			valid range [0, stripe_nsectors)
 * @bio_list_only:      Whether to use sectors inside the bio list only.
 *
 * The read/modify/write code wants to reuse the original bio page as much
 * as possible, and only use stripe_sectors as fallback.
 *
 * Return NULL if bio_list_only is set but the specified sector has no
 * coresponding bio.
 */
static phys_addr_t *sector_paddrs_in_rbio(struct btrfs_raid_bio *rbio,
					  int stripe_nr, int sector_nr,
					  bool bio_list_only)
{
	phys_addr_t *ret = NULL;
	const int index = rbio_paddr_index(rbio, stripe_nr, sector_nr, 0);

	ASSERT(index >= 0 && index < rbio->nr_sectors * rbio->sector_nsteps);

	scoped_guard(spinlock, &rbio->bio_list_lock) {
		if (rbio->bio_paddrs[index] != INVALID_PADDR || bio_list_only) {
			/* Don't return sector without a valid page pointer */
			if (rbio->bio_paddrs[index] != INVALID_PADDR)
				ret = &rbio->bio_paddrs[index];
			return ret;
		}
	}
	return &rbio->stripe_paddrs[index];
}

/*
 * Similar to sector_paddr_in_rbio(), but with extra consideration for
 * bs > ps cases, where we can have multiple steps for a fs block.
 */
static phys_addr_t sector_paddr_in_rbio(struct btrfs_raid_bio *rbio,
					int stripe_nr, int sector_nr, int step_nr,
					bool bio_list_only)
{
	phys_addr_t ret = INVALID_PADDR;
	const int index = rbio_paddr_index(rbio, stripe_nr, sector_nr, step_nr);

	ASSERT(index >= 0 && index < rbio->nr_sectors * rbio->sector_nsteps);

	scoped_guard(spinlock, &rbio->bio_list_lock) {
		if (rbio->bio_paddrs[index] != INVALID_PADDR || bio_list_only) {
			/* Don't return sector without a valid page pointer */
			if (rbio->bio_paddrs[index] != INVALID_PADDR)
				ret = rbio->bio_paddrs[index];
			return ret;
		}
	}
	return rbio->stripe_paddrs[index];
}

/*
 * allocation and initial setup for the btrfs_raid_bio.  Not
 * this does not allocate any pages for rbio->pages.
 */
static struct btrfs_raid_bio *alloc_rbio(struct btrfs_fs_info *fs_info,
					 struct btrfs_io_context *bioc)
{
	const unsigned int real_stripes = bioc->num_stripes - bioc->replace_nr_stripes;
	const unsigned int stripe_npages = BTRFS_STRIPE_LEN >> PAGE_SHIFT;
	const unsigned int num_pages = stripe_npages * real_stripes;
	const unsigned int stripe_nsectors =
		BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits;
	const unsigned int num_sectors = stripe_nsectors * real_stripes;
	const unsigned int step = min(fs_info->sectorsize, PAGE_SIZE);
	const unsigned int sector_nsteps = fs_info->sectorsize / step;
	struct btrfs_raid_bio *rbio;

	/*
	 * For bs <= ps cases, ps must be aligned to bs.
	 * For bs > ps cases, bs must be aligned to ps.
	 */
	ASSERT(IS_ALIGNED(PAGE_SIZE, fs_info->sectorsize) ||
	       IS_ALIGNED(fs_info->sectorsize, PAGE_SIZE));
	/*
	 * Our current stripe len should be fixed to 64k thus stripe_nsectors
	 * (at most 16) should be no larger than BITS_PER_LONG.
	 */
	ASSERT(stripe_nsectors <= BITS_PER_LONG);

	/*
	 * Real stripes must be between 2 (2 disks RAID5, aka RAID1) and 256
	 * (limited by u8).
	 */
	ASSERT(real_stripes >= 2);
	ASSERT(real_stripes <= U8_MAX);

	rbio = kzalloc_obj(*rbio, GFP_NOFS);
	if (!rbio)
		return ERR_PTR(-ENOMEM);
	rbio->stripe_pages = kzalloc_objs(struct page *, num_pages, GFP_NOFS);
	rbio->bio_paddrs = kzalloc_objs(phys_addr_t,
					num_sectors * sector_nsteps, GFP_NOFS);
	rbio->stripe_paddrs = kzalloc_objs(phys_addr_t,
					   num_sectors * sector_nsteps,
					   GFP_NOFS);
	rbio->finish_pointers = kcalloc(real_stripes, sizeof(void *), GFP_NOFS);
	rbio->error_bitmap = bitmap_zalloc(num_sectors, GFP_NOFS);
	rbio->stripe_uptodate_bitmap = bitmap_zalloc(num_sectors, GFP_NOFS);
	rbio->verified_bitmap = bitmap_zalloc(num_sectors, GFP_NOFS);
	rbio->repair_bitmap = bitmap_zalloc(num_sectors, GFP_NOFS);

	if (!rbio->stripe_pages || !rbio->bio_paddrs || !rbio->stripe_paddrs ||
	    !rbio->finish_pointers || !rbio->error_bitmap || !rbio->stripe_uptodate_bitmap ||
	    !rbio->repair_bitmap) {
		free_raid_bio_pointers(rbio);
		kfree(rbio);
		return ERR_PTR(-ENOMEM);
	}
	for (int i = 0; i < num_sectors * sector_nsteps; i++) {
		rbio->stripe_paddrs[i] = INVALID_PADDR;
		rbio->bio_paddrs[i] = INVALID_PADDR;
	}

	bio_list_init(&rbio->bio_list);
	init_waitqueue_head(&rbio->io_wait);
	INIT_LIST_HEAD(&rbio->plug_list);
	spin_lock_init(&rbio->bio_list_lock);
	INIT_LIST_HEAD(&rbio->stripe_cache);
	INIT_LIST_HEAD(&rbio->hash_list);
	btrfs_get_bioc(bioc);
	rbio->bioc = bioc;
	rbio->nr_pages = num_pages;
	rbio->nr_sectors = num_sectors;
	rbio->real_stripes = real_stripes;
	rbio->stripe_npages = stripe_npages;
	rbio->stripe_nsectors = stripe_nsectors;
	rbio->sector_nsteps = sector_nsteps;
	refcount_set(&rbio->refs, 1);
	atomic_set(&rbio->stripes_pending, 0);

	ASSERT(btrfs_nr_parity_stripes(bioc->map_type));
	rbio->nr_data = real_stripes - btrfs_nr_parity_stripes(bioc->map_type);
	ASSERT(rbio->nr_data > 0);

	return rbio;
}

/* allocate pages for all the stripes in the bio, including parity */
static int alloc_rbio_pages(struct btrfs_raid_bio *rbio)
{
	int ret;

	ret = btrfs_alloc_page_array(rbio->nr_pages, rbio->stripe_pages, GFP_NOFS);
	if (ret < 0)
		return ret;
	/* Mapping all sectors */
	index_stripe_sectors(rbio);
	return 0;
}

/* only allocate pages for p/q stripes */
static int alloc_rbio_parity_pages(struct btrfs_raid_bio *rbio)
{
	const int data_pages = rbio->nr_data * rbio->stripe_npages;
	int ret;

	ret = btrfs_alloc_page_array(rbio->nr_pages - data_pages,
				     rbio->stripe_pages + data_pages, GFP_NOFS);
	if (ret < 0)
		return ret;

	index_stripe_sectors(rbio);
	return 0;
}

/*
 * Return the total number of errors found in the vertical stripe of @sector_nr.
 *
 * @faila and @failb will also be updated to the first and second stripe
 * number of the errors.
 */
/*
 * How many faults a vertical stripe of this rbio can survive.
 *
 * Deliberately not bioc->max_errors: while a device replace is running,
 * btrfs_map_block() appends the replace target as an extra stripe and raises
 * bioc->max_errors to cover a failure of that copy (see
 * handle_ops_on_dev_replace()).  The RAID56 code does not track the target in
 * error_bitmap (it only has bits for the real stripes, see alloc_rbio()), so
 * counting real failures against the raised threshold would tolerate one more
 * real device failure than the profile does, and a stripe that lost its
 * redundancy would be reported as written successfully.
 */
static inline int rbio_max_errors(const struct btrfs_raid_bio *rbio)
{
	return rbio->real_stripes - rbio->nr_data;
}

/*
 * Faults of a vertical stripe after a write: the sectors whose I/O failed,
 * plus the sectors of a device that is missing altogether.
 *
 * get_rbio_vertical_errors() only sees sectors this rbio issued I/O for.  A
 * missing device is a fault of the whole vertical stripe: the sectors of it
 * that this write did not cover hold committed data which is now only
 * reconstructable from the parity this write just rewrote.  Ignoring that
 * would let a write complete successfully after leaving a vertical stripe
 * with more faults than the profile can survive.
 */
static int get_rbio_vertical_faults(struct btrfs_raid_bio *rbio, int sector_nr)
{
	int faults = 0;

	for (int stripe_nr = 0; stripe_nr < rbio->real_stripes; stripe_nr++) {
		if (!rbio->bioc->stripes[stripe_nr].dev->bdev ||
		    test_bit(stripe_nr * rbio->stripe_nsectors + sector_nr,
			     rbio->error_bitmap))
			faults++;
	}
	return faults;
}

static int get_rbio_vertical_errors(struct btrfs_raid_bio *rbio, int sector_nr,
				    int *faila, int *failb)
{
	int stripe_nr;
	int found_errors = 0;

	if (faila || failb) {
		/*
		 * Both @faila and @failb should be valid pointers if any of
		 * them is specified.
		 */
		ASSERT(faila && failb);
		*faila = -1;
		*failb = -1;
	}

	for (stripe_nr = 0; stripe_nr < rbio->real_stripes; stripe_nr++) {
		int total_sector_nr = stripe_nr * rbio->stripe_nsectors + sector_nr;

		if (test_bit(total_sector_nr, rbio->error_bitmap)) {
			found_errors++;
			if (faila) {
				/* Update faila and failb. */
				if (*faila < 0)
					*faila = stripe_nr;
				else if (*failb < 0)
					*failb = stripe_nr;
			}
		}
	}
	return found_errors;
}

static int bio_add_paddrs(struct bio *bio, phys_addr_t *paddrs, unsigned int nr_steps,
			  unsigned int step)
{
	int added = 0;
	int ret;

	for (int i = 0; i < nr_steps; i++) {
		ret = bio_add_page(bio, phys_to_page(paddrs[i]), step,
				   offset_in_page(paddrs[i]));
		if (ret != step)
			goto revert;
		added += ret;
	}
	return added;
revert:
	/*
	 * We don't need to revert the bvec, as the bio will be submitted immediately,
	 * as long as the size is reduced the extra bvec will not be accessed.
	 */
	bio->bi_iter.bi_size -= added;
	return 0;
}

/*
 * Add a single sector @sector into our list of bios for IO.
 *
 * Return 0 if everything went well.
 * Return <0 for error, and no byte will be added to @rbio.
 */
static int rbio_add_io_paddrs(struct btrfs_raid_bio *rbio, struct bio_list *bio_list,
			      phys_addr_t *paddrs, unsigned int stripe_nr,
			      unsigned int sector_nr, enum req_op op)
{
	const u32 sectorsize = rbio->bioc->fs_info->sectorsize;
	const u32 step = min(sectorsize, PAGE_SIZE);
	struct bio *last = bio_list->tail;
	int ret;
	struct bio *bio;
	struct btrfs_io_stripe *stripe;
	u64 disk_start;

	/*
	 * Note: here stripe_nr has taken device replace into consideration,
	 * thus it can be larger than rbio->real_stripe.
	 * So here we check against bioc->num_stripes, not rbio->real_stripes.
	 */
	ASSERT_RBIO_STRIPE(stripe_nr >= 0 && stripe_nr < rbio->bioc->num_stripes,
			   rbio, stripe_nr);
	ASSERT_RBIO_SECTOR(sector_nr >= 0 && sector_nr < rbio->stripe_nsectors,
			   rbio, sector_nr);
	ASSERT(paddrs != NULL);

	stripe = &rbio->bioc->stripes[stripe_nr];
	disk_start = stripe->physical + sector_nr * sectorsize;

	/* if the device is missing, just fail this stripe */
	if (!stripe->dev->bdev) {
		int found_errors;

		set_bit(stripe_nr * rbio->stripe_nsectors + sector_nr,
			rbio->error_bitmap);

		/* Check if we have reached tolerance early. */
		found_errors = get_rbio_vertical_errors(rbio, sector_nr,
							NULL, NULL);
		if (unlikely(found_errors > rbio_max_errors(rbio)))
			return -EIO;
		return 0;
	}

	/* see if we can add this page onto our existing bio */
	if (last) {
		u64 last_end = last->bi_iter.bi_sector << SECTOR_SHIFT;
		last_end += last->bi_iter.bi_size;

		/*
		 * we can't merge these if they are from different
		 * devices or if they are not contiguous
		 */
		if (last_end == disk_start && !last->bi_status &&
		    last->bi_bdev == stripe->dev->bdev) {
			ret = bio_add_paddrs(last, paddrs, rbio->sector_nsteps, step);
			if (ret == sectorsize)
				return 0;
		}
	}

	/* put a new bio on the list */
	bio = bio_alloc(stripe->dev->bdev,
			max(BTRFS_STRIPE_LEN >> PAGE_SHIFT, 1),
			op, GFP_NOFS);
	bio->bi_iter.bi_sector = disk_start >> SECTOR_SHIFT;
	bio->bi_private = rbio;

	ret = bio_add_paddrs(bio, paddrs, rbio->sector_nsteps, step);
	ASSERT(ret == sectorsize);
	bio_list_add(bio_list, bio);
	return 0;
}

static void index_one_bio(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u32 step = min(fs_info->sectorsize, PAGE_SIZE);
	const u32 step_bits = min(fs_info->sectorsize_bits, PAGE_SHIFT);
	struct bvec_iter iter = bio->bi_iter;
	phys_addr_t paddr;
	u32 offset = (bio->bi_iter.bi_sector << SECTOR_SHIFT) -
		     rbio->bioc->full_stripe_logical;

	btrfs_bio_for_each_block(paddr, bio, &iter, step) {
		unsigned int index = (offset >> step_bits);

		rbio->bio_paddrs[index] = paddr;
		offset += step;
	}
}

/*
 * helper function to walk our bio list and populate the bio_pages array with
 * the result.  This seems expensive, but it is faster than constantly
 * searching through the bio list as we setup the IO in finish_rmw or stripe
 * reconstruction.
 *
 * This must be called before you trust the answers from page_in_rbio
 */
static void index_rbio_pages(struct btrfs_raid_bio *rbio)
{
	struct bio *bio;

	spin_lock(&rbio->bio_list_lock);
	bio_list_for_each(bio, &rbio->bio_list)
		index_one_bio(rbio, bio);

	spin_unlock(&rbio->bio_list_lock);
}

static void bio_get_trace_info(struct btrfs_raid_bio *rbio, struct bio *bio,
			       struct raid56_bio_trace_info *trace_info)
{
	const struct btrfs_io_context *bioc = rbio->bioc;
	int i;

	ASSERT(bioc);

	/* We rely on bio->bi_bdev to find the stripe number. */
	if (!bio->bi_bdev)
		goto not_found;

	for (i = 0; i < bioc->num_stripes; i++) {
		if (bio->bi_bdev != bioc->stripes[i].dev->bdev)
			continue;
		trace_info->stripe_nr = i;
		trace_info->devid = bioc->stripes[i].dev->devid;
		trace_info->offset = (bio->bi_iter.bi_sector << SECTOR_SHIFT) -
				     bioc->stripes[i].physical;
		return;
	}

not_found:
	trace_info->devid = -1;
	trace_info->offset = -1;
	trace_info->stripe_nr = -1;
}

static inline void bio_list_put(struct bio_list *bio_list)
{
	struct bio *bio;

	while ((bio = bio_list_pop(bio_list)))
		bio_put(bio);
}

static void assert_rbio(struct btrfs_raid_bio *rbio)
{
	if (!IS_ENABLED(CONFIG_BTRFS_ASSERT))
		return;

	/*
	 * At least two stripes (2 disks RAID5), and since real_stripes is U8,
	 * we won't go beyond 256 disks anyway.
	 */
	ASSERT_RBIO(rbio->real_stripes >= 2, rbio);
	ASSERT_RBIO(rbio->nr_data > 0, rbio);

	/*
	 * This is another check to make sure nr data stripes is smaller
	 * than total stripes.
	 */
	ASSERT_RBIO(rbio->nr_data < rbio->real_stripes, rbio);
}

void *kmap_local_paddr(phys_addr_t paddr)
{
	/* The sector pointer must have a page mapped to it. */
	ASSERT(paddr != INVALID_PADDR);

	return kmap_local_page(phys_to_page(paddr)) + offset_in_page(paddr);
}

static void generate_pq_vertical_step(struct btrfs_raid_bio *rbio, unsigned int sector_nr,
				      unsigned int step_nr)
{
	void **pointers = rbio->finish_pointers;
	const u32 step = min(rbio->bioc->fs_info->sectorsize, PAGE_SIZE);
	int stripe;
	const bool has_qstripe = rbio->bioc->map_type & BTRFS_BLOCK_GROUP_RAID6;

	/* First collect one sector from each data stripe */
	for (stripe = 0; stripe < rbio->nr_data; stripe++)
		pointers[stripe] = kmap_local_paddr(
				sector_paddr_in_rbio(rbio, stripe, sector_nr, step_nr, 0));

	/* Then add the parity stripe */
	pointers[stripe++] = kmap_local_paddr(rbio_pstripe_paddr(rbio, sector_nr, step_nr));

	if (has_qstripe) {
		/*
		 * RAID6, add the qstripe and call the library function
		 * to fill in our p/q
		 */
		pointers[stripe++] = kmap_local_paddr(
				rbio_qstripe_paddr(rbio, sector_nr, step_nr));

		assert_rbio(rbio);
		raid6_gen_syndrome(rbio->real_stripes, step, pointers);
	} else {
		/* raid5 */
		memcpy(pointers[rbio->nr_data], pointers[0], step);
		xor_gen(pointers[rbio->nr_data], pointers + 1, rbio->nr_data - 1,
				step);
	}
	for (stripe = stripe - 1; stripe >= 0; stripe--)
		kunmap_local(pointers[stripe]);
}

/* Generate PQ for one vertical stripe. */
static void generate_pq_vertical(struct btrfs_raid_bio *rbio, int sectornr)
{
	const bool has_qstripe = (rbio->bioc->map_type & BTRFS_BLOCK_GROUP_RAID6);

	for (int i = 0; i < rbio->sector_nsteps; i++)
		generate_pq_vertical_step(rbio, sectornr, i);

	set_bit(rbio_sector_index(rbio, rbio->nr_data, sectornr),
		rbio->stripe_uptodate_bitmap);
	if (has_qstripe)
		set_bit(rbio_sector_index(rbio, rbio->nr_data + 1, sectornr),
			rbio->stripe_uptodate_bitmap);
}

static int rmw_assemble_write_bios(struct btrfs_raid_bio *rbio,
				   struct bio_list *bio_list, int crash_point)
{
	/* The total sector number inside the full stripe. */
	int total_sector_nr;
	struct btrfs_raid56_write_stats *stats;
	u64 resident = 0;
	u64 supplied = 0;
	int sectornr;
	int stripe;
	int ret;

	ASSERT(bio_list_size(bio_list) == 0);

	/* We should have at least one data sector. */
	ASSERT(bitmap_weight(&rbio->dbitmap, rbio->stripe_nsectors));

	/* Accounting only, see struct btrfs_raid56_write_stats. */
	stats = &rbio->bioc->fs_info->raid56_write_stats;

	/*
	 * Reset errors, as we may have errors inherited from from degraded
	 * write.
	 */
	bitmap_clear(rbio->error_bitmap, 0, rbio->nr_sectors);

	/*
	 * Start assembly.  Make bios for everything from the higher layers (the
	 * bio_list in our rbio) and our P/Q.  Ignore everything else.
	 */
	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		phys_addr_t *paddrs;

		stripe = total_sector_nr / rbio->stripe_nsectors;
		sectornr = total_sector_nr % rbio->stripe_nsectors;

		/* This vertical stripe has no data, skip it. */
		if (!test_bit(sectornr, &rbio->dbitmap))
			continue;

		if (stripe < rbio->nr_data) {
			paddrs = sector_paddrs_in_rbio(rbio, stripe, sectornr, 1);
			/*
			 * A data sector of a touched vertical stripe that this
			 * write does not supply: it was read off the disks (or
			 * reconstructed) to recompute the parity, and its
			 * redundancy is what a crash in the middle of these
			 * writes puts at risk.
			 *
			 * Unless it was proven wrong on disk and rebuilt, in
			 * which case it goes back out with the parity it was
			 * just folded into -- see rmw_prepare_repair().
			 */
			if (paddrs == NULL &&
			    test_bit(total_sector_nr, rbio->repair_bitmap))
				paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
			if (paddrs == NULL) {
				resident++;
				continue;
			}
			supplied++;
			/* Testing: pretend the data writes never reached the disk. */
			if (unlikely(crash_point == 2))
				continue;
		} else {
			paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
			/* Testing: pretend the P/Q writes never reached the disk. */
			if (unlikely(crash_point == 1 || crash_point == 3))
				continue;
		}

		ret = rbio_add_io_paddrs(rbio, bio_list, paddrs, stripe,
					 sectornr, REQ_OP_WRITE);
		if (ret)
			goto error;
	}

	if (!rbio_is_full(rbio)) {
		atomic64_inc(&stats->partial);
		atomic64_add(bitmap_weight(&rbio->dbitmap, rbio->stripe_nsectors),
			     &stats->partial_vstripes);
		atomic64_add(supplied, &stats->partial_sectors);
		atomic64_add(resident, &stats->partial_resident);
	} else if (test_bit(RBIO_INPLACE_BIT, &rbio->flags)) {
		atomic64_inc(&stats->inplace);
	} else {
		atomic64_inc(&stats->full);
	}

	if (likely(!rbio->bioc->replace_nr_stripes))
		return 0;

	/*
	 * Make a copy for the replace target device.
	 *
	 * Thus the source stripe number (in replace_stripe_src) should be valid.
	 */
	ASSERT(rbio->bioc->replace_stripe_src >= 0);

	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		phys_addr_t *paddrs;

		stripe = total_sector_nr / rbio->stripe_nsectors;
		sectornr = total_sector_nr % rbio->stripe_nsectors;

		/*
		 * For RAID56, there is only one device that can be replaced,
		 * and replace_stripe_src[0] indicates the stripe number we
		 * need to copy from.
		 */
		if (stripe != rbio->bioc->replace_stripe_src) {
			/*
			 * We can skip the whole stripe completely, note
			 * total_sector_nr will be increased by one anyway.
			 */
			ASSERT(sectornr == 0);
			total_sector_nr += rbio->stripe_nsectors - 1;
			continue;
		}

		/* This vertical stripe has no data, skip it. */
		if (!test_bit(sectornr, &rbio->dbitmap))
			continue;

		if (stripe < rbio->nr_data) {
			paddrs = sector_paddrs_in_rbio(rbio, stripe, sectornr, 1);
			if (paddrs == NULL &&
			    test_bit(total_sector_nr, rbio->repair_bitmap))
				paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
			if (paddrs == NULL)
				continue;
		} else {
			paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
		}

		ret = rbio_add_io_paddrs(rbio, bio_list, paddrs,
					 rbio->real_stripes,
					 sectornr, REQ_OP_WRITE);
		if (ret)
			goto error;
	}

	return 0;
error:
	bio_list_put(bio_list);
	return -EIO;
}

static void set_rbio_range_error(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	u32 offset = (bio->bi_iter.bi_sector << SECTOR_SHIFT) -
		     rbio->bioc->full_stripe_logical;
	int total_nr_sector = offset >> fs_info->sectorsize_bits;

	ASSERT(total_nr_sector < rbio->nr_data * rbio->stripe_nsectors);

	bitmap_set(rbio->error_bitmap, total_nr_sector,
		   bio->bi_iter.bi_size >> fs_info->sectorsize_bits);

	/*
	 * Special handling for raid56_alloc_missing_rbio() used by
	 * scrub/replace.  Unlike call path in raid56_parity_recover(), they
	 * pass an empty bio here.  Thus we have to find out the missing device
	 * and mark the stripe error instead.
	 */
	if (bio->bi_iter.bi_size == 0) {
		bool found_missing = false;
		int stripe_nr;

		for (stripe_nr = 0; stripe_nr < rbio->real_stripes; stripe_nr++) {
			if (!rbio->bioc->stripes[stripe_nr].dev->bdev) {
				found_missing = true;
				bitmap_set(rbio->error_bitmap,
					   stripe_nr * rbio->stripe_nsectors,
					   rbio->stripe_nsectors);
			}
		}
		ASSERT(found_missing);
	}
}

/*
 * Return the index inside the rbio->stripe_sectors[] array.
 *
 * Return -1 if not found.
 */
static int find_stripe_sector_nr(struct btrfs_raid_bio *rbio, phys_addr_t paddr)
{
	for (int i = 0; i < rbio->nr_sectors; i++) {
		if (rbio->stripe_paddrs[i * rbio->sector_nsteps] == paddr)
			return i;
	}
	return -1;
}

/*
 * this sets each page in the bio uptodate.  It should only be used on private
 * rbio pages, nothing that comes in from the higher layers
 */
static void set_bio_pages_uptodate(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	const u32 sectorsize = rbio->bioc->fs_info->sectorsize;
	const u32 step = min(sectorsize, PAGE_SIZE);
	u32 offset = 0;
	phys_addr_t paddr;

	ASSERT(!bio_flagged(bio, BIO_CLONED));

	btrfs_bio_for_each_block_all(paddr, bio, step) {
		/* Hitting the first step of a sector. */
		if (IS_ALIGNED(offset, sectorsize)) {
			int sector_nr = find_stripe_sector_nr(rbio, paddr);

			ASSERT(sector_nr >= 0);
			if (sector_nr >= 0)
				set_bit(sector_nr, rbio->stripe_uptodate_bitmap);
		}
		offset += step;
	}
}

static int get_bio_sector_nr(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	phys_addr_t bvec_paddr = bvec_phys(bio_first_bvec_all(bio));
	int i;

	for (i = 0; i < rbio->nr_sectors; i++) {
		if (rbio->stripe_paddrs[i * rbio->sector_nsteps] == bvec_paddr)
			break;
		if (rbio->bio_paddrs[i * rbio->sector_nsteps] == bvec_paddr)
			break;
	}
	ASSERT(i < rbio->nr_sectors);
	return i;
}

static void rbio_update_error_bitmap(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	int total_sector_nr = get_bio_sector_nr(rbio, bio);
	const u32 bio_size = bio_get_size(bio);

	/*
	 * Since we can have multiple bios touching the error_bitmap, we cannot
	 * call bitmap_set() without protection.
	 *
	 * Instead use set_bit() for each bit, as set_bit() itself is atomic.
	 */
	for (int i = total_sector_nr; i < total_sector_nr +
	     (bio_size >> rbio->bioc->fs_info->sectorsize_bits); i++)
		set_bit(i, rbio->error_bitmap);
}

/*
 * Treat the sectors the write-intent log records as stale the way a failed
 * checksum is treated, so they are rebuilt from the parity rather than
 * believed -- but only when the rebuild can actually be done.
 *
 * Three conditions, each of which is a counterexample the state-machine model
 * produced from an earlier version that did not have it
 * (tools/testing/btrfs/scrub_policy_model.py --all-readers):
 *
 *  - every stale column of the FULL STRIPE is marked, not only the sectors
 *    in the bio at hand.  A parity describes the whole stripe; rebuilding one
 *    column while another column it was computed from is also stale returns a
 *    value nothing ever committed.
 *
 *  - a parity the log records as stale is marked failed too, so the rebuild
 *    does not use it.  It describes an older data vector, and rebuilding from
 *    it undoes an acknowledged write just as surely as folding a stale sector
 *    into a new parity does.
 *
 *  - if all of that would not fit inside the profile's fault tolerance,
 *    nothing is marked at all.  Without a checksum a rebuild that runs out of
 *    equations does not fail loudly, it returns garbage; leaving the stale
 *    sector alone is what the code did before this record existed, and being
 *    no worse than that in the cases it cannot help is what makes it safe to
 *    have on.
 */
static void mark_stale_sectors(struct btrfs_raid_bio *rbio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const int nr_parity = rbio->real_stripes - rbio->nr_data;
	struct btrfs_wib_stripe_state st;
	u64 failed = 0, add = 0, want = 0;
	int nr_failed = 0;

	/*
	 * Wider than the masks below can express.  Returning leaves the read
	 * exactly as it was before this record existed -- the sectors on disk
	 * are returned as found -- which is upstream's behaviour and is safe
	 * in the sense that it invents nothing.  Count it so that a stripe
	 * this wide does not silently opt out of the protection.
	 */
	if (rbio->real_stripes > 64) {
		if (fs_info->wib)
			atomic64_inc(&fs_info->wib->stat_read_ambiguous);
		return;
	}
	if (!btrfs_wib_stripe_state(fs_info, rbio->bioc->full_stripe_logical,
				    rbio->nr_data, nr_parity, &st))
		return;

	/*
	 * The defect this function was rewritten to fix, on a switch, so the
	 * test that demonstrates the fix can also demonstrate the failure
	 * without a second kernel.  See raid56_stale_read_legacy.
	 */
	if (btrfs_raid56_stale_read_legacy()) {
		for (int i = 0; i < rbio->nr_data; i++) {
			const int first = i * rbio->stripe_nsectors;

			if (!(st.stale_cols & BIT_ULL(i)))
				continue;
			for (int nr = 0; nr < rbio->stripe_nsectors; nr++)
				set_bit(first + nr, rbio->error_bitmap);
		}
		return;
	}

	/*
	 * Stripes already known bad; their faults count against the budget.
	 * A missing device is among them: rbio_add_io_paddrs() sets its bits
	 * while the read bios are assembled, which is before any of them can
	 * complete and bring us here.
	 *
	 * Several bios complete concurrently and share this bitmap, so the
	 * count can be one short of what another bio is in the middle of
	 * recording.  Two callers reading the same log state compute the same
	 * @add and set_bit() is idempotent, so the union is right either way;
	 * what a concurrent real IO error can do is push the total past the
	 * tolerance after this check passed.  The recovery then fails the read
	 * rather than returning the stale sector -- an error where there would
	 * have been silently wrong data, which is the safe direction to be
	 * wrong in, and not worth a lock on a bio completion path to avoid.
	 */
	for (int i = 0; i < rbio->real_stripes; i++) {
		const int first = i * rbio->stripe_nsectors;
		const int end = first + rbio->stripe_nsectors;

		if (find_next_bit(rbio->error_bitmap, end, first) < end) {
			failed |= BIT_ULL(i);
			nr_failed++;
		}
	}

	/*
	 * @want is every member the record names; @add is the part of it not
	 * already counted as failed, which is what the budget has to find room
	 * for.  But mark all of @want, not just @add.  A column counts as failed
	 * here as soon as ANY of its sectors has an error bit -- a read of one
	 * of its sectors, a checksum mismatch -- and its other sectors are
	 * exactly as stale as before.  Leaving them unmarked sent every other
	 * vertical stripe of the column to a single-parity rebuild that folded
	 * the stale sector in: on RAID6 the Q cross-check then failed the whole
	 * read, on RAID5 it returned a value nothing committed.
	 */
	for (int i = 0; i < rbio->nr_data; i++)
		if (st.stale_cols & BIT_ULL(i))
			want |= BIT_ULL(i);
	for (int p = 0; p < nr_parity; p++)
		if (st.bad_parity & BIT(p))
			want |= BIT_ULL(rbio->nr_data + p);
	add = want & ~failed;

	if (unlikely(btrfs_raid56_trace_reads()))
		btrfs_info(fs_info,
"raid56: RTRACE stale full stripe %llu op %d failed 0x%llx stale 0x%llx bad_parity 0x%x add 0x%llx",
			   rbio->bioc->full_stripe_logical, rbio->operation, failed,
			   st.stale_cols, st.bad_parity, add);

	/*
	 * The log names more members than the surviving parity can rebuild.
	 * Return the sectors as they are on disk rather than reconstructing a
	 * value nothing ever committed -- see the budget argument above.
	 *
	 * @add is what makes this an ambiguity rather than an ordinary
	 * unrecoverable read: without it the read was already over budget on
	 * real IO errors alone and the log had nothing to add.  This is the
	 * commonest place in the system where an ambiguous stripe is found,
	 * and until this counter existed it left no trace at all.
	 */
	if (nr_failed + hweight64(add) > nr_parity) {
		if (add && fs_info->wib)
			atomic64_inc(&fs_info->wib->stat_read_ambiguous);
		if (add)
			set_bit(RBIO_STALE_AMBIGUOUS_BIT, &rbio->flags);
		return;
	}

	/*
	 * Several bios share this bitmap, so set_bit() one at a time rather
	 * than bitmap_set(); see the comment in rbio_update_error_bitmap().
	 */
	for (int i = 0; i < rbio->real_stripes; i++) {
		const int first = i * rbio->stripe_nsectors;

		if (!(want & BIT_ULL(i)))
			continue;
		for (int nr = 0; nr < rbio->stripe_nsectors; nr++) {
			/*
			 * Leave a sector that has a checksum to its checksum.
			 * That is the stronger evidence and it is about to be
			 * checked a few lines below: if the sector really is
			 * stale it fails and gets rebuilt anyway, and if it
			 * passes then the record is a false positive and
			 * overriding a verified sector with a reconstruction
			 * would be a guess.  This record exists for the
			 * sectors that have nothing else -- see
			 * rmw_update_stale_data(), which sets it for any
			 * failed sub-stripe write and not only for those.
			 */
			if (i < rbio->nr_data && rbio->csum_bitmap &&
			    test_bit(first + nr, rbio->csum_bitmap))
				continue;
			set_bit(first + nr, rbio->error_bitmap);
			/* Proven wrong on disk by the record: may be written back. */
			set_bit(first + nr, rbio->repair_bitmap);
		}
	}
}

/* Verify the data sectors at read time. */
static void verify_bio_data_sectors(struct btrfs_raid_bio *rbio,
				    struct bio *bio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u32 step = min(fs_info->sectorsize, PAGE_SIZE);
	const u32 nr_steps = rbio->sector_nsteps;
	int total_sector_nr = get_bio_sector_nr(rbio, bio);
	u32 offset = 0;
	phys_addr_t paddrs[BTRFS_MAX_BLOCKSIZE / PAGE_SIZE];
	phys_addr_t paddr;

	/* P/Q stripes, they have no data csum to verify against. */
	if (total_sector_nr >= rbio->nr_data * rbio->stripe_nsectors)
		return;

	/*
	 * Sectors a failed write left stale, which the write-intent log
	 * records.  This runs BEFORE the csum_bitmap check below, and must:
	 * fill_data_csums() frees the bitmap outright when nothing in the
	 * stripe has a checksum, which is precisely the nodatacow case this
	 * exists for.  Marked here, a stale sector is treated exactly as a
	 * checksum mismatch would be, and is rebuilt from the parity instead
	 * of believed -- so the next parity this write computes keeps the
	 * value the caller was told is on the disk, rather than the one the
	 * device did not take.
	 */
	if (unlikely(btrfs_wib_any_stale(fs_info)))
		mark_stale_sectors(rbio);

	/* No data csum for the whole stripe, nothing else to verify. */
	if (!rbio->csum_bitmap || !rbio->csum_buf)
		return;

	btrfs_bio_for_each_block_all(paddr, bio, step) {
		u8 csum_buf[BTRFS_CSUM_SIZE];
		u8 *expected_csum;

		paddrs[(offset / step) % nr_steps] = paddr;
		offset += step;

		/* Not yet covering the full fs block, continue to the next step. */
		if (!IS_ALIGNED(offset, fs_info->sectorsize))
			continue;

		/* No csum for this sector, skip to the next sector. */
		if (!test_bit(total_sector_nr, rbio->csum_bitmap)) {
			total_sector_nr++;
			continue;
		}

		expected_csum = rbio->csum_buf + total_sector_nr * fs_info->csum_size;
		btrfs_calculate_block_csum_pages(fs_info, paddrs, csum_buf);
		if (unlikely(memcmp(csum_buf, expected_csum, fs_info->csum_size) != 0))
			set_bit(total_sector_nr, rbio->error_bitmap);
		else if (rbio->verified_bitmap)
			set_bit(total_sector_nr, rbio->verified_bitmap);
		total_sector_nr++;
	}
}

/*
 * The device a stripe bio was submitted to, or NULL if it cannot be told.
 *
 * Stripe bios are submitted with submit_bio() and never carry a btrfs_bio, so
 * the block device is the only link back.  Every stripe of a bioc is on a
 * different device, so this is unambiguous.
 */
static struct btrfs_device *rbio_bio_device(struct btrfs_raid_bio *rbio,
					    const struct bio *bio)
{
	const struct btrfs_io_context *bioc = rbio->bioc;

	if (!bio->bi_bdev)
		return NULL;
	for (int i = 0; i < bioc->num_stripes; i++) {
		if (bio->bi_bdev == bioc->stripes[i].dev->bdev)
			return bioc->stripes[i].dev;
	}
	return NULL;
}

/*
 * Is this the copy of a stripe that goes to the device replace target?
 *
 * Such a bio carries the pages of the stripe it duplicates, so its failure
 * must not be recorded in error_bitmap: that would count against the source
 * device's fault budget although the source took the write.  The target is
 * not part of the RAID redundancy yet, its failure only fails the replace.
 */
static bool rbio_bio_is_replace_target(struct btrfs_raid_bio *rbio,
				       const struct bio *bio)
{
	const struct btrfs_io_context *bioc = rbio->bioc;

	if (!bioc->replace_nr_stripes || !bio->bi_bdev)
		return false;
	for (int i = rbio->real_stripes; i < bioc->num_stripes; i++) {
		if (bio->bi_bdev == bioc->stripes[i].dev->bdev)
			return true;
	}
	return false;
}

/*
 * Account a failed stripe I/O to the device, so that it shows up in
 * "btrfs device stats" and in the log.  The RAID56 paths submit their bios
 * directly and therefore bypass the accounting btrfs_submit_bio() does for
 * every other I/O.
 */
static void rbio_account_io_error(struct btrfs_raid_bio *rbio, struct bio *bio)
{
	struct btrfs_device *dev = rbio_bio_device(rbio, bio);

	if (!dev)
		return;
	if (bio_op(bio) == REQ_OP_WRITE)
		btrfs_dev_stat_inc_and_print(dev, BTRFS_DEV_STAT_WRITE_ERRS);
	else if (!(bio->bi_opf & REQ_RAHEAD))
		btrfs_dev_stat_inc_and_print(dev, BTRFS_DEV_STAT_READ_ERRS);
	btrfs_warn_rl(rbio->bioc->fs_info,
	"raid56: %s error on %s, full stripe %llu sector %llu, status %d",
		      bio_op(bio) == REQ_OP_WRITE ? "write" : "read",
		      btrfs_dev_name(dev), rbio->bioc->full_stripe_logical,
		      (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT,
		      blk_status_to_errno(bio->bi_status));
}

static void raid_wait_read_end_io(struct bio *bio)
{
	struct btrfs_raid_bio *rbio = bio->bi_private;

	if (bio->bi_status) {
		rbio_account_io_error(rbio, bio);
		rbio_update_error_bitmap(rbio, bio);
	} else {
		set_bio_pages_uptodate(rbio, bio);
		verify_bio_data_sectors(rbio, bio);
	}

	bio_put(bio);
	if (atomic_dec_and_test(&rbio->stripes_pending))
		wake_up(&rbio->io_wait);
}

static void submit_read_wait_bio_list(struct btrfs_raid_bio *rbio,
			     struct bio_list *bio_list)
{
	struct bio *bio;

	atomic_set(&rbio->stripes_pending, bio_list_size(bio_list));
	while ((bio = bio_list_pop(bio_list))) {
		bio->bi_end_io = raid_wait_read_end_io;

		if (trace_raid56_read_enabled()) {
			struct raid56_bio_trace_info trace_info = { 0 };

			bio_get_trace_info(rbio, bio, &trace_info);
			trace_call__raid56_read(rbio, bio, &trace_info);
		}
		submit_bio(bio);
	}

	wait_event(rbio->io_wait, atomic_read(&rbio->stripes_pending) == 0);
}

static int alloc_rbio_data_pages(struct btrfs_raid_bio *rbio)
{
	const int data_pages = rbio->nr_data * rbio->stripe_npages;
	int ret;

	ret = btrfs_alloc_page_array(data_pages, rbio->stripe_pages, GFP_NOFS);
	if (ret < 0)
		return ret;

	index_stripe_sectors(rbio);
	return 0;
}

/*
 * We use plugging call backs to collect full stripes.
 * Any time we get a partial stripe write while plugged
 * we collect it into a list.  When the unplug comes down,
 * we sort the list by logical block number and merge
 * everything we can into the same rbios
 */
struct btrfs_plug_cb {
	struct blk_plug_cb cb;
	struct btrfs_fs_info *info;
	struct list_head rbio_list;
};

/*
 * rbios on the plug list are sorted for easier merging.
 */
static int plug_cmp(void *priv, const struct list_head *a,
		    const struct list_head *b)
{
	const struct btrfs_raid_bio *ra = container_of(a, struct btrfs_raid_bio,
						       plug_list);
	const struct btrfs_raid_bio *rb = container_of(b, struct btrfs_raid_bio,
						       plug_list);
	u64 a_sector = ra->bio_list.head->bi_iter.bi_sector;
	u64 b_sector = rb->bio_list.head->bi_iter.bi_sector;

	if (a_sector < b_sector)
		return -1;
	if (a_sector > b_sector)
		return 1;
	return 0;
}

static void raid_unplug(struct blk_plug_cb *cb, bool from_schedule)
{
	struct btrfs_plug_cb *plug = container_of(cb, struct btrfs_plug_cb, cb);
	struct btrfs_raid_bio *cur;
	struct btrfs_raid_bio *last = NULL;

	list_sort(NULL, &plug->rbio_list, plug_cmp);

	while (!list_empty(&plug->rbio_list)) {
		cur = list_first_entry(&plug->rbio_list,
				       struct btrfs_raid_bio, plug_list);
		list_del_init(&cur->plug_list);

		if (rbio_is_full(cur)) {
			/* We have a full stripe, queue it down. */
			start_async_work(cur, rmw_rbio_work);
			continue;
		}
		if (last) {
			if (rbio_can_merge(last, cur)) {
				merge_rbio(last, cur);
				free_raid_bio(cur);
				continue;
			}
			start_async_work(last, rmw_rbio_work);
		}
		last = cur;
	}
	if (last)
		start_async_work(last, rmw_rbio_work);
	kfree(plug);
}

/* Add the original bio into rbio->bio_list, and update rbio::dbitmap. */
static void rbio_add_bio(struct btrfs_raid_bio *rbio, struct bio *orig_bio)
{
	const struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u64 orig_logical = orig_bio->bi_iter.bi_sector << SECTOR_SHIFT;
	const u64 full_stripe_start = rbio->bioc->full_stripe_logical;
	const u32 orig_len = orig_bio->bi_iter.bi_size;
	const u32 sectorsize = fs_info->sectorsize;
	u64 cur_logical;

	ASSERT_RBIO_LOGICAL(orig_logical >= full_stripe_start &&
			    orig_logical + orig_len <= full_stripe_start +
			    rbio->nr_data * BTRFS_STRIPE_LEN,
			    rbio, orig_logical);

	bio_list_add(&rbio->bio_list, orig_bio);
	rbio->bio_list_bytes += orig_bio->bi_iter.bi_size;

	/* Update the dbitmap. */
	for (cur_logical = orig_logical; cur_logical < orig_logical + orig_len;
	     cur_logical += sectorsize) {
		int bit = ((u32)(cur_logical - full_stripe_start) >>
			   fs_info->sectorsize_bits) % rbio->stripe_nsectors;

		set_bit(bit, &rbio->dbitmap);
	}
}

/*
 * our main entry point for writes from the rest of the FS.
 */
/*
 * Does this data write overwrite sectors that are already referenced?
 * Only nodatacow and preallocated extents are written in place; everything
 * else (COW data, metadata, tree log blocks) goes to unreferenced space.
 */
static bool bio_writes_in_place(struct bio *bio)
{
	struct btrfs_bio *bbio = btrfs_bio(bio);

	if (!bbio->inode || !is_data_inode(bbio->inode))
		return false;
	if (!bbio->ordered)
		return false;
	return test_bit(BTRFS_ORDERED_NOCOW, &bbio->ordered->flags) ||
	       test_bit(BTRFS_ORDERED_PREALLOC, &bbio->ordered->flags);
}

void raid56_parity_write(struct bio *bio, struct btrfs_io_context *bioc)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	struct btrfs_raid_bio *rbio;
	struct btrfs_plug_cb *plug = NULL;
	struct blk_plug_cb *cb;

	rbio = alloc_rbio(fs_info, bioc);
	if (IS_ERR(rbio)) {
		bio->bi_status = errno_to_blk_status(PTR_ERR(rbio));
		bio_endio(bio);
		return;
	}
	rbio->operation = BTRFS_RBIO_WRITE;
	if (bio_writes_in_place(bio))
		set_bit(RBIO_INPLACE_BIT, &rbio->flags);
	rbio_add_bio(rbio, bio);

	/*
	 * Don't plug on full rbios, just get them out the door
	 * as quickly as we can
	 */
	if (!rbio_is_full(rbio)) {
		cb = blk_check_plugged(raid_unplug, fs_info, sizeof(*plug));
		if (cb) {
			plug = container_of(cb, struct btrfs_plug_cb, cb);
			if (!plug->info) {
				plug->info = fs_info;
				INIT_LIST_HEAD(&plug->rbio_list);
			}
			list_add_tail(&rbio->plug_list, &plug->rbio_list);
			return;
		}
	}

	/*
	 * Either we don't have any existing plug, or we're doing a full stripe,
	 * queue the rmw work now.
	 */
	start_async_work(rbio, rmw_rbio_work);
}

/*
 * Check a sector against the checksum tree.
 *
 * Returns 1 when the sector was compared against a checksum and matched, 0
 * when there was no checksum to compare it against, and -EIO on a mismatch.
 * The caller has to tell those apart: a reconstruction that nothing verified
 * is not a reconstruction that is known good.
 */
static int verify_one_sector(struct btrfs_raid_bio *rbio,
			     int stripe_nr, int sector_nr)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	phys_addr_t *paddrs;
	u8 csum_buf[BTRFS_CSUM_SIZE];
	u8 *csum_expected;

	if (!rbio->csum_bitmap || !rbio->csum_buf)
		return 0;

	/* No way to verify P/Q as they are not covered by data csum. */
	if (stripe_nr >= rbio->nr_data)
		return 0;

	/* A data block group can hold nodatasum extents with no checksum. */
	if (!test_bit(stripe_nr * rbio->stripe_nsectors + sector_nr,
		      rbio->csum_bitmap))
		return 0;
	/*
	 * If we're rebuilding a read, we have to use pages from the
	 * bio list if possible.
	 */
	if (rbio->operation == BTRFS_RBIO_READ_REBUILD) {
		paddrs = sector_paddrs_in_rbio(rbio, stripe_nr, sector_nr, 0);
	} else {
		paddrs = rbio_stripe_paddrs(rbio, stripe_nr, sector_nr);
	}

	csum_expected = rbio->csum_buf +
			(stripe_nr * rbio->stripe_nsectors + sector_nr) *
			fs_info->csum_size;
	btrfs_calculate_block_csum_pages(fs_info, paddrs, csum_buf);
	if (unlikely(memcmp(csum_buf, csum_expected, fs_info->csum_size) != 0))
		return -EIO;
	return 1;
}

/*
 * Record whether a rebuilt DATA sector that is actually being RETURNED was
 * vouched for by anything.
 *
 * @ret is verify_one_sector()'s: 1 checked and correct, 0 not checked at all.
 * The second is the dangerous one -- the sector is handed to the caller as the
 * file's content on the strength of a parity that may not describe it.
 *
 * Only sectors in the caller's bio are counted.  A recovery rebuilds every
 * column of the vertical stripe, including sectors that hold no extent, and
 * those have no checksum for the honest reason that there is nothing there to
 * checksum.  Counting them would bury the sectors that matter under free
 * space and make the number say nothing.
 */
static void count_recover_verification(struct btrfs_raid_bio *rbio, int stripe_nr,
				       int sector_nr, int ret)
{
	struct btrfs_raid56_write_stats *st;

	if (stripe_nr >= rbio->nr_data)
		return;
	if (!sector_paddrs_in_rbio(rbio, stripe_nr, sector_nr, true))
		return;
	st = &rbio->bioc->fs_info->raid56_write_stats;
	if (ret > 0) {
		atomic64_inc(&st->recover_verified);
		if (rbio->verified_bitmap)
			set_bit(rbio_sector_index(rbio, stripe_nr, sector_nr),
				rbio->verified_bitmap);
		return;
	}
	atomic64_inc(&st->recover_unverified);
	if (!rbio->csum_bitmap || !rbio->csum_buf)
		atomic64_inc(&st->recover_unverified_nobitmap);
	else
		atomic64_inc(&st->recover_unverified_nobit);
	/*
	 * Print the sector's own logical address, not just the stripe's: the
	 * question this has to answer is WHOSE sector it is, and that needs an
	 * address btrfs_ioctl_logical_to_ino() can resolve.
	 */
	btrfs_warn_rl(rbio->bioc->fs_info,
"raid56: UNVERIFIED_REBUILD logical %llu (full stripe %llu col %d) returned from the parity with nothing to check it against",
		      rbio->bioc->full_stripe_logical +
		      ((u64)stripe_nr << BTRFS_STRIPE_LEN_SHIFT) +
		      ((u64)sector_nr << rbio->bioc->fs_info->sectorsize_bits),
		      rbio->bioc->full_stripe_logical, stripe_nr);
}

/*
 * Cross-check a RAID6 single-erasure reconstruction against the second
 * syndrome.
 *
 * A vertical stripe with one failed data sector is rebuilt from P alone, the
 * same way RAID5 does it, and Q is never consulted.  If P is stale - a lost
 * or torn parity write, or bit rot on the parity device - the rebuild is
 * silently wrong.  For a sector with a checksum that is caught, but metadata
 * on a RAID6 profile and nodatasum data have none, and the RMW path then
 * folds the wrong sector into both the new P and the new Q, destroying the
 * correct content that Q still described.
 *
 * Recompute the syndrome over the reconstructed data and compare the Q it
 * produces with the Q on disk.  They match only if the reconstruction agrees
 * with both parities.  Returns 0 when consistent, -EIO when not.
 */
static int recover_verify_q(struct btrfs_raid_bio *rbio, int sector_nr,
			    void **pointers, void **unmap_array, void *scratch_p,
			    void *scratch_q)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u32 step = min(fs_info->sectorsize, PAGE_SIZE);
	const int qstripe = rbio->real_stripes - 1;
	int ret = 0;

	for (int step_nr = 0; step_nr < rbio->sector_nsteps; step_nr++) {
		for (int stripe_nr = 0; stripe_nr <= qstripe; stripe_nr++) {
			phys_addr_t paddr;

			if (rbio->operation == BTRFS_RBIO_READ_REBUILD)
				paddr = sector_paddr_in_rbio(rbio, stripe_nr,
							     sector_nr, step_nr, 0);
			else
				paddr = rbio_stripe_paddr(rbio, stripe_nr,
							  sector_nr, step_nr);
			pointers[stripe_nr] = kmap_local_paddr(paddr);
			unmap_array[stripe_nr] = pointers[stripe_nr];
		}

		/*
		 * gen_syndrome() writes P and Q into the last two entries, so
		 * point them at scratch and keep what is on disk intact.
		 */
		pointers[rbio->nr_data] = scratch_p;
		pointers[qstripe] = scratch_q;
		raid6_gen_syndrome(rbio->real_stripes, step, pointers);

		if (unlikely(memcmp(scratch_q, unmap_array[qstripe], step) != 0))
			ret = -EIO;

		for (int stripe_nr = qstripe; stripe_nr >= 0; stripe_nr--)
			kunmap_local(unmap_array[stripe_nr]);
		if (ret)
			break;
	}

	if (ret)
		btrfs_warn_rl(fs_info,
	"raid56: rebuild of full stripe %llu sector %d does not match the Q syndrome, the parity is stale or corrupt",
			      rbio->bioc->full_stripe_logical, sector_nr);
	return ret;
}

static void recover_vertical_step(struct btrfs_raid_bio *rbio,
				  unsigned int sector_nr,
				  unsigned int step_nr,
				  int faila, int failb,
				  void **pointers, void **unmap_array)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u32 step = min(fs_info->sectorsize, PAGE_SIZE);
	int stripe_nr;

	ASSERT(step_nr < rbio->sector_nsteps);
	ASSERT(sector_nr < rbio->stripe_nsectors);

	/*
	 * Setup our array of pointers with sectors from each stripe
	 *
	 * NOTE: store a duplicate array of pointers to preserve the
	 * pointer order.
	 */
	for (stripe_nr = 0; stripe_nr < rbio->real_stripes; stripe_nr++) {
		phys_addr_t paddr;

		/*
		 * If we're rebuilding a read, we have to use pages from the
		 * bio list if possible.
		 */
		if (rbio->operation == BTRFS_RBIO_READ_REBUILD) {
			paddr = sector_paddr_in_rbio(rbio, stripe_nr, sector_nr, step_nr, 0);
		} else {
			paddr = rbio_stripe_paddr(rbio, stripe_nr, sector_nr, step_nr);
		}
		pointers[stripe_nr] = kmap_local_paddr(paddr);
		unmap_array[stripe_nr] = pointers[stripe_nr];
	}

	/* All raid6 handling here */
	if (rbio->bioc->map_type & BTRFS_BLOCK_GROUP_RAID6) {
		/* Single failure, rebuild from parity raid5 style */
		if (failb < 0) {
			if (faila == rbio->nr_data)
				/*
				 * Just the P stripe has failed, without
				 * a bad data or Q stripe.
				 * We have nothing to do, just skip the
				 * recovery for this stripe.
				 */
				goto cleanup;
			/*
			 * a single failure in raid6 is rebuilt
			 * in the pstripe code below
			 */
			goto pstripe;
		}

		/*
		 * If the q stripe is failed, do a pstripe reconstruction from
		 * the xors.
		 * If both the q stripe and the P stripe are failed, we're
		 * here due to a crc mismatch and we can't give them the
		 * data they want.
		 */
		if (failb == rbio->real_stripes - 1) {
			if (faila == rbio->real_stripes - 2)
				/*
				 * Only P and Q are corrupted.
				 * We only care about data stripes recovery,
				 * can skip this vertical stripe.
				 */
				goto cleanup;
			/*
			 * Otherwise we have one bad data stripe and
			 * a good P stripe.  raid5!
			 */
			goto pstripe;
		}

		if (failb == rbio->real_stripes - 2) {
			raid6_recov_datap(rbio->real_stripes, step,
					  faila, pointers);
		} else {
			raid6_recov_2data(rbio->real_stripes, step,
					  faila, failb, pointers);
		}
	} else {
		void *p;

		/* Rebuild from P stripe here (raid5 or raid6). */
		ASSERT(failb == -1);
pstripe:
		/* Copy parity block into failed block to start with */
		memcpy(pointers[faila], pointers[rbio->nr_data], step);

		/* Rearrange the pointer array */
		p = pointers[faila];
		for (stripe_nr = faila; stripe_nr < rbio->nr_data - 1;
		     stripe_nr++)
			pointers[stripe_nr] = pointers[stripe_nr + 1];
		pointers[rbio->nr_data - 1] = p;

		/* Xor in the rest */
		xor_gen(p, pointers, rbio->nr_data - 1, step);
	}

cleanup:
	for (stripe_nr = rbio->real_stripes - 1; stripe_nr >= 0; stripe_nr--)
		kunmap_local(unmap_array[stripe_nr]);
}

/*
 * Recover a vertical stripe specified by @sector_nr.
 * @*pointers are the pre-allocated pointers by the caller, so we don't
 * need to allocate/free the pointers again and again.
 */
static int recover_vertical(struct btrfs_raid_bio *rbio, int sector_nr,
			    void **pointers, void **unmap_array,
			    void *scratch_p, void *scratch_q)
{
	int found_errors;
	int faila;
	int failb;
	int ret = 0;

	/*
	 * Now we just use bitmap to mark the horizontal stripes in
	 * which we have data when doing parity scrub.
	 */
	if (rbio->operation == BTRFS_RBIO_PARITY_SCRUB &&
	    !test_bit(sector_nr, &rbio->dbitmap))
		return 0;

	found_errors = get_rbio_vertical_errors(rbio, sector_nr, &faila,
						&failb);
	/*
	 * No errors in the vertical stripe, skip it.  Can happen for recovery
	 * which only part of a stripe failed csum check.
	 */
	if (!found_errors)
		return 0;

	if (unlikely(found_errors > rbio_max_errors(rbio)))
		return -EIO;

	for (int i = 0; i < rbio->sector_nsteps; i++)
		recover_vertical_step(rbio, sector_nr, i, faila, failb,
					    pointers, unmap_array);
	if (faila >= 0) {
		ret = verify_one_sector(rbio, faila, sector_nr);
		if (ret < 0)
			return ret;
		count_recover_verification(rbio, faila, sector_nr, ret);

		/*
		 * A RAID6 stripe with a single failed data sector was rebuilt
		 * from P alone.  If no checksum vouched for the result, ask
		 * the second syndrome instead of trusting P blindly.
		 */
		if (ret == 0 && scratch_p && failb < 0 && faila < rbio->nr_data) {
			ret = recover_verify_q(rbio, sector_nr, pointers,
					       unmap_array, scratch_p, scratch_q);
			if (ret < 0)
				return ret;
		}

		set_bit(rbio_sector_index(rbio, faila, sector_nr),
			rbio->stripe_uptodate_bitmap);
	}
	if (failb >= 0) {
		ret = verify_one_sector(rbio, failb, sector_nr);
		if (ret < 0)
			return ret;
		count_recover_verification(rbio, failb, sector_nr, ret);

		set_bit(rbio_sector_index(rbio, failb, sector_nr),
			rbio->stripe_uptodate_bitmap);
	}
	return 0;
}

static int recover_sectors(struct btrfs_raid_bio *rbio)
{
	const bool has_qstripe = rbio->real_stripes - rbio->nr_data == 2;
	void **pointers = NULL;
	void **unmap_array = NULL;
	void *scratch_p = NULL;
	void *scratch_q = NULL;
	int sectornr;
	int ret = 0;

	/*
	 * @pointers array stores the pointer for each sector.
	 *
	 * @unmap_array stores copy of pointers that does not get reordered
	 * during reconstruction so that kunmap_local works.
	 */
	pointers = kzalloc_objs(void *, rbio->real_stripes, GFP_NOFS);
	unmap_array = kzalloc_objs(void *, rbio->real_stripes, GFP_NOFS);
	if (!pointers || !unmap_array) {
		ret = -ENOMEM;
		goto out;
	}
	/* Scratch for the RAID6 syndrome cross-check, see recover_verify_q(). */
	if (has_qstripe) {
		scratch_p = (void *)__get_free_page(GFP_NOFS);
		scratch_q = (void *)__get_free_page(GFP_NOFS);
		if (!scratch_p || !scratch_q) {
			ret = -ENOMEM;
			goto out;
		}
	}

	if (rbio->operation == BTRFS_RBIO_READ_REBUILD) {
		spin_lock(&rbio->bio_list_lock);
		set_bit(RBIO_RMW_LOCKED_BIT, &rbio->flags);
		spin_unlock(&rbio->bio_list_lock);
	}

	index_rbio_pages(rbio);

	for (sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++) {
		ret = recover_vertical(rbio, sectornr, pointers, unmap_array,
				       scratch_p, scratch_q);
		if (ret < 0)
			break;
	}

out:
	free_page((unsigned long)scratch_p);
	free_page((unsigned long)scratch_q);
	kfree(pointers);
	kfree(unmap_array);
	return ret;
}

static void recover_rbio(struct btrfs_raid_bio *rbio)
{
	struct bio_list bio_list = BIO_EMPTY_LIST;
	int total_sector_nr;
	int ret = 0;

	/*
	 * Either we're doing recover for a read failure or degraded write,
	 * caller should have set error bitmap correctly.
	 */
	ASSERT(bitmap_weight(rbio->error_bitmap, rbio->nr_sectors));

	/*
	 * Fill the data csums so the reconstruction can be checked against
	 * them.  verify_one_sector() is called on every rebuilt sector by
	 * recover_vertical(), and has a branch for BTRFS_RBIO_READ_REBUILD --
	 * so this path was built to verify and simply was never armed: nothing
	 * populated csum_bitmap for a read, and verify_one_sector() returns 0
	 * immediately when it is NULL.
	 *
	 * Without this, a reconstruction is handed back unchecked.  That is
	 * fine when the parity describes the data, and silent corruption when
	 * it does not -- which is the state a lost write leaves behind.  A
	 * degraded read then returns, with no error, bytes that were never
	 * written: measured at 6 files per pair of runs in
	 * tools/testing/btrfs/uml/dmfail34.sh, stable across re-reads and
	 * varying with which device is missing, on data that
	 * BTRFS_IOC_GET_CSUMS confirms is checksummed.
	 *
	 * rmw_read_wait_recover() has always done this for the read half of a
	 * read-modify-write.  The read path is the one that hands the bytes to
	 * a caller, so it needed it more.
	 */
	fill_data_csums(rbio);

	/* For recovery, we need to read all sectors including P/Q. */
	ret = alloc_rbio_pages(rbio);
	if (ret < 0)
		goto out;

	index_rbio_pages(rbio);

	/*
	 * Read everything that hasn't failed. However this time we will
	 * not trust any cached sector.
	 * As we may read out some stale data but higher layer is not reading
	 * that stale part.
	 *
	 * So here we always re-read everything in recovery path.
	 */
	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		int stripe = total_sector_nr / rbio->stripe_nsectors;
		int sectornr = total_sector_nr % rbio->stripe_nsectors;
		phys_addr_t *paddrs;

		/*
		 * Skip the range which has error.  It can be a range which is
		 * marked error (for csum mismatch), or it can be a missing
		 * device.
		 */
		if (!rbio->bioc->stripes[stripe].dev->bdev ||
		    test_bit(total_sector_nr, rbio->error_bitmap)) {
			/*
			 * Also set the error bit for missing device, which
			 * may not yet have its error bit set.
			 */
			set_bit(total_sector_nr, rbio->error_bitmap);
			continue;
		}

		paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, stripe,
					 sectornr, REQ_OP_READ);
		if (ret < 0) {
			bio_list_put(&bio_list);
			goto out;
		}
	}

	submit_read_wait_bio_list(rbio, &bio_list);
	ret = recover_sectors(rbio);
out:
	rbio_orig_end_io(rbio, errno_to_blk_status(ret));
}

static void recover_rbio_work(struct work_struct *work)
{
	struct btrfs_raid_bio *rbio;

	rbio = container_of(work, struct btrfs_raid_bio, work);
	if (!lock_stripe_add(rbio))
		recover_rbio(rbio);
}

static void recover_rbio_work_locked(struct work_struct *work)
{
	recover_rbio(container_of(work, struct btrfs_raid_bio, work));
}

static void set_rbio_raid6_extra_error(struct btrfs_raid_bio *rbio, int mirror_num)
{
	bool found = false;
	int sector_nr;

	/*
	 * This is for RAID6 extra recovery tries, thus mirror number should
	 * be large than 2.
	 * Mirror 1 means read from data stripes. Mirror 2 means rebuild using
	 * RAID5 methods.
	 */
	ASSERT(mirror_num > 2);
	for (sector_nr = 0; sector_nr < rbio->stripe_nsectors; sector_nr++) {
		int found_errors;
		int faila;
		int failb;

		found_errors = get_rbio_vertical_errors(rbio, sector_nr,
							 &faila, &failb);
		/* This vertical stripe doesn't have errors. */
		if (!found_errors)
			continue;

		/*
		 * If we found errors, there should be only one error marked
		 * by previous set_rbio_range_error().
		 */
		ASSERT(found_errors == 1);
		found = true;

		/* Now select another stripe to mark as error. */
		failb = rbio->real_stripes - (mirror_num - 1);
		if (failb <= faila)
			failb--;

		/* Set the extra bit in error bitmap. */
		if (failb >= 0)
			set_bit(failb * rbio->stripe_nsectors + sector_nr,
				rbio->error_bitmap);
	}

	/* We should found at least one vertical stripe with error.*/
	ASSERT(found);
}

/*
 * the main entry point for reads from the higher layers.  This
 * is really only called when the normal read path had a failure,
 * so we assume the bio they send down corresponds to a failed part
 * of the drive.
 */
void raid56_parity_recover(struct bio *bio, struct btrfs_io_context *bioc,
			   int mirror_num)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	struct btrfs_raid_bio *rbio;

	rbio = alloc_rbio(fs_info, bioc);
	if (IS_ERR(rbio)) {
		bio->bi_status = errno_to_blk_status(PTR_ERR(rbio));
		bio_endio(bio);
		return;
	}

	rbio->operation = BTRFS_RBIO_READ_REBUILD;
	rbio_add_bio(rbio, bio);

	set_rbio_range_error(rbio, bio);

	/*
	 * Loop retry:
	 * for 'mirror == 2', reconstruct from all other stripes.
	 * for 'mirror_num > 2', select a stripe to fail on every retry.
	 */
	if (mirror_num > 2)
		set_rbio_raid6_extra_error(rbio, mirror_num);

	start_async_work(rbio, recover_rbio_work);
}

static void fill_data_csums(struct btrfs_raid_bio *rbio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	struct btrfs_root *csum_root;
	const u64 start = rbio->bioc->full_stripe_logical;
	const u32 len = (rbio->nr_data * rbio->stripe_nsectors) <<
			fs_info->sectorsize_bits;
	int ret;

	/* The rbio should not have its csum buffer initialized. */
	ASSERT(!rbio->csum_buf && !rbio->csum_bitmap);

	/*
	 * Skip the csum search if:
	 *
	 * - The rbio doesn't belong to data block groups
	 *   Then we are doing IO for tree blocks, no need to search csums.
	 *
	 * - The rbio belongs to mixed block groups
	 *   This is to avoid deadlock, as we're already holding the full
	 *   stripe lock, if we trigger a metadata read, and it needs to do
	 *   raid56 recovery, we will deadlock.
	 */
	if (!(rbio->bioc->map_type & BTRFS_BLOCK_GROUP_DATA) ||
	    rbio->bioc->map_type & BTRFS_BLOCK_GROUP_METADATA)
		return;

	rbio->csum_buf = kzalloc(rbio->nr_data * rbio->stripe_nsectors *
				 fs_info->csum_size, GFP_NOFS);
	rbio->csum_bitmap = bitmap_zalloc(rbio->nr_data * rbio->stripe_nsectors,
					  GFP_NOFS);
	if (!rbio->csum_buf || !rbio->csum_bitmap) {
		ret = -ENOMEM;
		goto error;
	}

	csum_root = btrfs_csum_root(fs_info, rbio->bioc->full_stripe_logical);
	if (unlikely(!csum_root)) {
		btrfs_err(fs_info,
			  "missing csum root for extent at bytenr %llu",
			  rbio->bioc->full_stripe_logical);
		ret = -EUCLEAN;
		goto error;
	}

	ret = btrfs_lookup_csums_bitmap(csum_root, NULL, start, start + len - 1,
					rbio->csum_buf, rbio->csum_bitmap);
	if (ret < 0)
		goto error;
	if (bitmap_empty(rbio->csum_bitmap, len >> fs_info->sectorsize_bits))
		goto no_csum;
	return;

error:
	/*
	 * We failed to allocate memory or grab the csum, but it's not fatal,
	 * we can still continue.  But better to warn users that RMW is no
	 * longer safe for this particular sub-stripe write.
	 */
	btrfs_warn_rl(fs_info,
"sub-stripe write for full stripe %llu is not safe, failed to get csum: %d",
			rbio->bioc->full_stripe_logical, ret);
no_csum:
	kfree(rbio->csum_buf);
	bitmap_free(rbio->csum_bitmap);
	rbio->csum_buf = NULL;
	rbio->csum_bitmap = NULL;
}

static int rmw_read_wait_recover(struct btrfs_raid_bio *rbio)
{
	struct bio_list bio_list = BIO_EMPTY_LIST;
	int total_sector_nr;
	int ret = 0;

	/*
	 * Fill the data csums we need for data verification.  We need to fill
	 * the csum_bitmap/csum_buf first, as our endio function will try to
	 * verify the data sectors.
	 */
	fill_data_csums(rbio);

	/*
	 * Build a list of bios to read all sectors (including data and P/Q).
	 *
	 * This behavior is to compensate the later csum verification and recovery.
	 */
	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		int stripe = total_sector_nr / rbio->stripe_nsectors;
		int sectornr = total_sector_nr % rbio->stripe_nsectors;
		phys_addr_t *paddrs;

		paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, stripe,
					 sectornr, REQ_OP_READ);
		if (ret) {
			bio_list_put(&bio_list);
			return ret;
		}
	}

	/*
	 * We may or may not have any corrupted sectors (including missing dev
	 * and csum mismatch), just let recover_sectors() to handle them all.
	 */
	submit_read_wait_bio_list(rbio, &bio_list);
	return recover_sectors(rbio);
}

static void raid_wait_write_end_io(struct bio *bio)
{
	struct btrfs_raid_bio *rbio = bio->bi_private;

	if (bio->bi_status) {
		rbio_account_io_error(rbio, bio);
		/*
		 * A failed copy to the replace target says nothing about the
		 * redundancy of the stripe; the replace itself will fail.
		 */
		if (!rbio_bio_is_replace_target(rbio, bio))
			rbio_update_error_bitmap(rbio, bio);
	}
	bio_put(bio);
	if (atomic_dec_and_test(&rbio->stripes_pending))
		wake_up(&rbio->io_wait);
}

static void submit_write_bios(struct btrfs_raid_bio *rbio,
			      struct bio_list *bio_list)
{
	struct bio *bio;

	atomic_set(&rbio->stripes_pending, bio_list_size(bio_list));
	while ((bio = bio_list_pop(bio_list))) {
		bio->bi_end_io = raid_wait_write_end_io;

		if (trace_raid56_write_enabled()) {
			struct raid56_bio_trace_info trace_info = { 0 };

			bio_get_trace_info(rbio, bio, &trace_info);
			trace_call__raid56_write(rbio, bio, &trace_info);
		}
		submit_bio(bio);
	}
}

/*
 * To determine if we need to read any sector from the disk.
 * Should only be utilized in RMW path, to skip cached rbio.
 */
static bool need_read_stripe_sectors(struct btrfs_raid_bio *rbio)
{
	int i;

	for (i = 0; i < rbio->nr_data * rbio->stripe_nsectors; i++) {
		phys_addr_t paddr = rbio->stripe_paddrs[i * rbio->sector_nsteps];

		/*
		 * We have a sector which doesn't have page nor uptodate,
		 * thus this rbio can not be cached one, as cached one must
		 * have all its data sectors present and uptodate.
		 */
		if (paddr == INVALID_PADDR ||
		    !test_bit(i, rbio->stripe_uptodate_bitmap))
			return true;
	}
	return false;
}

/*
 * Retry the sectors whose write failed on a device that is still there.
 *
 * A write failure that stays within the profile's tolerance is accepted
 * today, and that costs the whole vertical stripe its redundancy:
 *
 *  - a failed parity write leaves the parity not matching the data, so every
 *    other sector of the stripe can no longer be reconstructed;
 *  - a failed data write leaves that sector stale on its device, so its
 *    committed content exists only inside the parity.
 *
 * Either way a single further fault anywhere in the stripe loses committed
 * data, until the next scrub repairs it.  One retry costs almost nothing and
 * turns the common transient failure (a bus reset, a command abort, a
 * momentarily overloaded device) back into a fully redundant stripe.
 *
 * Returns true if anything was retried, in which case error_bitmap has been
 * updated with the outcome of the retry.
 */
static bool rmw_retry_failed_sectors(struct btrfs_raid_bio *rbio)
{
	struct bio_list bio_list;
	unsigned int nr_retried = 0;
	int ret;

	bio_list_init(&bio_list);

	/*
	 * First pass: build the bios without touching error_bitmap.  If this
	 * has to be abandoned part way, every failed sector must still be
	 * recorded as failed, or the tolerance check below would count fewer
	 * faults than the stripe really has and accept the write.
	 */
	for (int stripe = 0; stripe < rbio->real_stripes; stripe++) {
		if (!rbio->bioc->stripes[stripe].dev->bdev)
			continue;
		for (int sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++) {
			const int index = stripe * rbio->stripe_nsectors + sectornr;
			phys_addr_t *paddrs;

			if (!test_bit(index, rbio->error_bitmap))
				continue;
			if (stripe < rbio->nr_data) {
				/* Only the data sectors this rbio wrote. */
				paddrs = sector_paddrs_in_rbio(rbio, stripe, sectornr, 1);
				if (!paddrs && test_bit(index, rbio->repair_bitmap))
					paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
				if (!paddrs)
					continue;
			} else {
				paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
			}
			ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, stripe,
						 sectornr, REQ_OP_WRITE);
			if (ret < 0) {
				bio_list_put(&bio_list);
				return false;
			}
			nr_retried++;
		}
	}

	if (!nr_retried)
		return false;

	/*
	 * Second pass, walking the same sectors: the retry is going ahead, so
	 * hand ownership of these bits to raid_wait_write_end_io(), which sets
	 * them again for whatever fails a second time.
	 */
	for (int stripe = 0; stripe < rbio->real_stripes; stripe++) {
		if (!rbio->bioc->stripes[stripe].dev->bdev)
			continue;
		for (int sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++) {
			const int index = stripe * rbio->stripe_nsectors + sectornr;

			if (!test_bit(index, rbio->error_bitmap))
				continue;
			if (stripe < rbio->nr_data &&
			    !sector_paddrs_in_rbio(rbio, stripe, sectornr, 1) &&
			    !test_bit(index, rbio->repair_bitmap))
				continue;
			clear_bit(index, rbio->error_bitmap);
		}
	}

	btrfs_warn_rl(rbio->bioc->fs_info,
		      "raid56: retrying %u failed sectors of full stripe %llu",
		      nr_retried, rbio->bioc->full_stripe_logical);
	submit_write_bios(rbio, &bio_list);
	wait_event(rbio->io_wait, atomic_read(&rbio->stripes_pending) == 0);
	return true;
}

/*
 * Tell the write-intent log which data stripes of this full stripe the write
 * failed to reach, so that the read path stops believing them.  Only data
 * stripes: a parity that was not written carries no logical address, and the
 * stripe stays recorded for the scrub either way.
 */
static void rmw_update_stale_data(struct btrfs_raid_bio *rbio, u64 full_stripe_start)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;

	for (int stripe = 0; stripe < rbio->nr_data; stripe++) {
		const int first = stripe * rbio->stripe_nsectors;
		const int end = first + rbio->stripe_nsectors;
		const u64 logical = full_stripe_start +
				    btrfs_stripe_nr_to_offset(stripe);
		bool supplied_any = false;
		bool supplied_all = true;

		/*
		 * Only the data stripes this rbio actually wrote have anything
		 * to say.  One it merely read to compute the parity is
		 * unchanged on disk, so neither marking it stale nor clearing
		 * a mark it already carries would be true -- and clearing it
		 * would turn the alarm off while the sector is still stale,
		 * which is worse than never having set it.
		 */
		for (int sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++) {
			const int index = first + sectornr;

			/*
			 * Written by this rbio: its own data, or a sector it
			 * rebuilt and wrote back (rmw_prepare_repair()).
			 */
			if (sector_paddrs_in_rbio(rbio, stripe, sectornr, 1) ||
			    test_bit(index, rbio->repair_bitmap))
				supplied_any = true;
			/*
			 * Not written, but read back and matched against its
			 * checksum: the disk holds what it should, which is all
			 * clearing the mark asserts.
			 */
			else if (!(rbio->verified_bitmap &&
				   test_bit(index, rbio->verified_bitmap)))
				supplied_all = false;
		}
		if (!supplied_any)
			continue;

		/*
		 * Any failed sector condemns the whole data stripe: the log's
		 * granularity is one BTRFS_STRIPE_LEN and being coarse here
		 * only costs a reconstruction that was not needed.
		 */
		if (find_next_bit(rbio->error_bitmap, end, first) < end) {
			btrfs_wib_mark_stale(fs_info, logical, BTRFS_STRIPE_LEN);
			continue;
		}

		/*
		 * Clearing is the opposite: it asserts health, and being coarse
		 * about THAT loses data.  One bit covers a whole 64KiB column
		 * (BTRFS_WIB_BLOCK_SHIFT == BTRFS_STRIPE_LEN_SHIFT) while a
		 * sub-stripe write touches only the vertical stripes it was
		 * given, so a single landed 4KiB write would otherwise clear a
		 * record describing fifteen sectors it never went near -- one
		 * of which is the sector whose acknowledged value exists
		 * nowhere but the parity.  Clear only when this write supplied
		 * the entire column, so that every sector the bit covers is
		 * one we just put there.
		 *
		 * btrfs_wib_done() refuses to clear @stale for exactly this
		 * reason one level up, where the range is the full stripe and
		 * the unit is the column.  Same argument, one level down.
		 */
		if (supplied_all)
			btrfs_wib_clear_stale(fs_info, logical, BTRFS_STRIPE_LEN);
	}
}

/*
 * Say, for each parity of this full stripe, whether it still describes the
 * data on disk.  A read-modify-write always writes every parity, so a write
 * that landed clears a record an earlier failure left behind and one that did
 * not sets it.
 *
 * The read and scrub paths need this to be a separate question from which
 * DATA is stale: rebuilding a data column out of a parity whose own write
 * failed returns the value that parity was last computed from, which is not
 * what was acknowledged.  See mark_stale_sectors().
 */
#ifdef CONFIG_BTRFS_DEBUG
/*
 * Treat every parity write as having failed, so that a stripe which already
 * has a named stale data column ends up with no usable parity either.  That is
 * the state a second, independent fault produces, and it is the one case the
 * repair path must decline: one column it cannot believe, nothing left to
 * rebuild it from.  Reaching it with real devices needs two of them failing at
 * once, which takes the filesystem read-only before the state exists -- so
 * inject the record the second fault would have written, and let the real
 * decision run on it.
 */
static bool stale_fake_bad_parity;
module_param_named(raid56_stale_fake_bad_parity, stale_fake_bad_parity, bool, 0644);
MODULE_PARM_DESC(raid56_stale_fake_bad_parity,
		 "Record every parity as not describing the data, to exercise the ambiguous case (testing only)");

/* See btrfs_raid56_stale_read_legacy() in volumes.h. */
static bool stale_read_legacy;
module_param_named(raid56_stale_read_legacy, stale_read_legacy, bool, 0644);
MODULE_PARM_DESC(raid56_stale_read_legacy,
		 "Mark stale sectors without checking that the rebuild fits inside the profile's tolerance, as the first version did (testing only: restores a known defect)");

bool btrfs_raid56_stale_read_legacy(void)
{
	return READ_ONCE(stale_read_legacy);
}

/* See btrfs_raid56_allow_nodatacow() in volumes.h. */
static bool allow_nodatacow;
module_param_named(raid56_allow_nodatacow, allow_nodatacow, bool, 0644);
MODULE_PARM_DESC(raid56_allow_nodatacow,
		 "Permit NODATACOW on RAID5/6, which is not safe (testing only)");

bool btrfs_raid56_allow_nodatacow(void)
{
	return READ_ONCE(allow_nodatacow);
}

/* See btrfs_raid56_scrub_trusts_rebuild() in volumes.h. */
static bool scrub_trusts_rebuild;
module_param_named(raid56_scrub_trusts_rebuild, scrub_trusts_rebuild, bool, 0644);
MODULE_PARM_DESC(raid56_scrub_trusts_rebuild,
		 "Let scrub write back a parity rebuild of unchecksummed data without being able to verify it, as it did before (testing only: restores a known defect)");

bool btrfs_raid56_scrub_trusts_rebuild(void)
{
	return READ_ONCE(scrub_trusts_rebuild);
}

/* See btrfs_raid56_trace_reads() in volumes.h. */
static bool trace_reads;
module_param_named(raid56_trace_reads, trace_reads, bool, 0644);
MODULE_PARM_DESC(raid56_trace_reads,
		 "Log every data sector a RAID5/6 read returns, and what checked it (testing only)");

#ifdef CONFIG_BTRFS_DEBUG
static bool read_ignores_stale;
module_param_named(raid56_read_ignores_stale, read_ignores_stale, bool, 0644);
MODULE_PARM_DESC(raid56_read_ignores_stale,
		 "Return a block the write-intent record names as stale as it is on disk when it has no checksum (testing only: restores the old behaviour)");
bool btrfs_raid56_read_ignores_stale(void)
{
	return READ_ONCE(read_ignores_stale);
}
#endif

bool btrfs_raid56_trace_reads(void)
{
	return READ_ONCE(trace_reads);
}
#endif

static void rmw_update_stale_parity(struct btrfs_raid_bio *rbio,
				    u64 full_stripe_start)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const int nr_parity = rbio->real_stripes - rbio->nr_data;

	for (int p = 0; p < nr_parity; p++) {
		const int first = (rbio->nr_data + p) * rbio->stripe_nsectors;
		const int end = first + rbio->stripe_nsectors;
		bool stale = find_next_bit(rbio->error_bitmap, end, first) < end;

#ifdef CONFIG_BTRFS_DEBUG
		if (unlikely(READ_ONCE(stale_fake_bad_parity)))
			stale = true;
#endif
		/*
		 * Clearing asserts that the whole parity column describes the
		 * data, and a sub-stripe write only rewrote the parity of the
		 * vertical stripes it touched.  One bit covers the column, so a
		 * 4KiB write that landed would otherwise clear the record of a
		 * parity write that failed fifteen vertical stripes away, and
		 * the next rebuild there would trust it.  Same argument as the
		 * data side in rmw_update_stale_data().
		 */
		if (!stale && !bitmap_full(&rbio->dbitmap, rbio->stripe_nsectors))
			continue;
		btrfs_wib_update_stale_parity(fs_info, full_stripe_start, p,
					      stale);
	}
}

#ifdef CONFIG_BTRFS_DEBUG
/*
 * Negative controls for uml/rmw_repair.sh: restore the read-modify-write as
 * it was before it repaired what it rebuilt, and before it refused to write
 * into a stripe the record cannot decide.
 */
static bool rmw_no_repair;
module_param_named(raid56_rmw_no_repair, rmw_no_repair, bool, 0644);
MODULE_PARM_DESC(raid56_rmw_no_repair,
		 "Do not write back sectors a read-modify-write rebuilt (testing only: restores the old behaviour)");
static bool rmw_no_refuse;
module_param_named(raid56_rmw_no_refuse, rmw_no_refuse, bool, 0644);
MODULE_PARM_DESC(raid56_rmw_no_refuse,
		 "Let a read-modify-write into an undecidable stripe go ahead (testing only: restores a known defect)");
static bool rmw_trust_cache;
module_param_named(raid56_rmw_trust_cache, rmw_trust_cache, bool, 0644);
MODULE_PARM_DESC(raid56_rmw_trust_cache,
		 "Serve a read-modify-write into a recorded stripe from the stripe cache (testing only: restores a known defect)");
#else
/*
 * Constants, not macros: the callers read these with READ_ONCE(), which needs
 * an lvalue.  The compiler folds them away all the same.
 */
static const bool rmw_no_repair;
static const bool rmw_no_refuse;
static const bool rmw_trust_cache;
#endif

/*
 * Does the write-intent record name any member of this full stripe as stale?
 * Lock-free while nothing anywhere is recorded stale.
 */
static bool rbio_stripe_recorded(struct btrfs_raid_bio *rbio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	struct btrfs_wib_stripe_state st;

	if (likely(!btrfs_wib_any_stale(fs_info)))
		return false;
	return btrfs_wib_stripe_state(fs_info, rbio->bioc->full_stripe_logical,
				      rbio->nr_data, rbio->real_stripes - rbio->nr_data,
				      &st);
}

/* Bit i set: a sector of bioc->stripes[i] failed.  For the alerts. */
static unsigned long rbio_failed_cols(const struct btrfs_raid_bio *rbio)
{
	unsigned long cols = 0;

	for (int i = 0; i < rbio->real_stripes && i < BITS_PER_LONG; i++) {
		const int first = i * rbio->stripe_nsectors;

		if (find_next_bit(rbio->error_bitmap, first + rbio->stripe_nsectors,
				  first) < first + rbio->stripe_nsectors)
			cols |= BIT(i);
	}
	return cols;
}

/*
 * Which members the record names stale, as bioc->stripes[] bits: data
 * column i is stripes[i], P and Q follow the data.  For the alerts.
 */
static unsigned long rbio_stale_cols(struct btrfs_raid_bio *rbio)
{
	struct btrfs_wib_stripe_state st;
	unsigned long cols;

	if (!btrfs_wib_stripe_state(rbio->bioc->fs_info, rbio->bioc->full_stripe_logical,
				    rbio->nr_data, rbio->real_stripes - rbio->nr_data,
				    &st))
		return 0;
	cols = st.stale_cols & (BIT(rbio->nr_data) - 1);
	cols |= (unsigned long)st.bad_parity << rbio->nr_data;
	return cols;
}

/*
 * A read-modify-write has just read the whole full stripe and rebuilt what it
 * could not believe.  Decide what of that goes back to the disks.
 *
 * Until now nothing did: the rebuilt sectors were folded into the new parity
 * and then dropped, so a sector left stale by an earlier failed write stayed
 * stale on its device, carried only by the parity, until a scrub came along --
 * and every write in between depended on the record being right.  The stripe
 * lock is already held and the right content is already in memory, so write
 * it back now.  That is the targeted repair of a stripe that errored, done by
 * the next writer, and it cannot be overtaken by another write to the stripe
 * because they all queue on the same lock.
 *
 * Only what was PROVEN wrong goes back (see @repair_bitmap): named by the
 * record, or rebuilt to a match against a checksum the on-disk copy failed.
 * A sector on a device that is not there has nowhere to go.
 *
 * Every vertical stripe that gets a sector back, or whose parity the record
 * names as not describing the data, gets its parity written too, so the
 * stripe is consistent where it was touched.
 */
static void rmw_prepare_repair(struct btrfs_raid_bio *rbio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	unsigned int nr = 0;

	for (int stripe = 0; stripe < rbio->real_stripes; stripe++) {
		const bool present = rbio->bioc->stripes[stripe].dev->bdev != NULL;

		for (int sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++) {
			const int index = rbio_sector_index(rbio, stripe, sectornr);
			bool proven;

			if (!test_bit(index, rbio->error_bitmap) || !present ||
			    READ_ONCE(rmw_no_repair)) {
				clear_bit(index, rbio->repair_bitmap);
				continue;
			}
			proven = test_bit(index, rbio->repair_bitmap);
			if (stripe < rbio->nr_data) {
				/*
				 * A sector this write supplies is kept too: the
				 * repair goes out first, ahead of the new data
				 * (rmw_repair_first()), and has to restore the
				 * whole column to what the parity still
				 * describes before anything changes the parity.
				 * rmw_assemble_write_bios() writes the new data
				 * over it, from the bio, either way.
				 */
				if (!proven && rbio->verified_bitmap &&
				    test_bit(index, rbio->verified_bitmap))
					proven = true;
				if (!proven)
					continue;
				set_bit(index, rbio->repair_bitmap);
				set_bit(sectornr, &rbio->dbitmap);
				nr++;
				continue;
			}
			/*
			 * A parity the record says is stale: rewrite the whole
			 * column, the record does not say which part.
			 */
			clear_bit(index, rbio->repair_bitmap);
			if (proven)
				bitmap_set(&rbio->dbitmap, 0, rbio->stripe_nsectors);
		}
	}
	if (nr)
		atomic64_add(nr, &fs_info->raid56_write_stats.rmw_repaired_sectors);
}

/*
 * The targeted repair of a full stripe a write left damaged.
 *
 * rmw_rbio() already repairs, under the stripe lock, whatever the record
 * proves stale in any stripe it writes to -- but only when something writes
 * to that stripe.  A stripe nobody writes to again would stay without its
 * redundancy until a scrub or the next mount.  So a write that hit a device
 * error queues its stripe here, and a repair rbio -- a read-modify-write with
 * no data of its own -- is submitted for it after a short delay, and retried
 * with a doubling delay while the device keeps refusing.  It queues on the
 * same stripe lock as every write, so no write can use the stripe between the
 * repair reading it and the repair landing; and any write that gets there
 * first does the same repair itself.
 *
 * Bounded: BTRFS_WIB_REPAIR_SLOTS stripes waiting, RAID56_REPAIR_MAX_TRIES
 * attempts each.  What does not fit or does not succeed stays recorded, which
 * is where it was before this existed: the next scrub or mount repairs it.
 */
#define RAID56_REPAIR_MAX_TRIES		6

static void rmw_rbio_work(struct work_struct *work);

static unsigned int repair_delay_ms = 1000;
#ifdef CONFIG_BTRFS_DEBUG
module_param_named(raid56_repair_delay_ms, repair_delay_ms, uint, 0644);
MODULE_PARM_DESC(raid56_repair_delay_ms,
		 "First delay before repairing a full stripe a write damaged, doubled per retry (testing only)");
static bool no_repair_on_fault;
module_param_named(raid56_no_repair_on_fault, no_repair_on_fault, bool, 0644);
MODULE_PARM_DESC(raid56_no_repair_on_fault,
		 "Do not queue a repair of a full stripe a write damaged (testing only: restores the old behaviour)");
static bool repair_keeps_record;
module_param_named(raid56_repair_keeps_record, repair_keeps_record, bool, 0644);
MODULE_PARM_DESC(raid56_repair_keeps_record,
		 "Leave the record of a full stripe after repairing it (testing only: restores the old behaviour)");
static bool repair_ignores_freeze;
module_param_named(raid56_repair_ignores_freeze, repair_ignores_freeze, bool, 0644);
MODULE_PARM_DESC(raid56_repair_ignores_freeze,
		 "Keep repairing while the filesystem is frozen (testing only: restores a known defect)");
static bool repair_no_pin;
module_param_named(raid56_repair_no_pin, repair_no_pin, bool, 0644);
MODULE_PARM_DESC(raid56_repair_no_pin,
		 "Neither freeze the block group under a repair nor drain repairs in relocation (testing only: restores a known defect)");
/*
 * Hold the next repair this long between deciding what to write and writing
 * it, so a test can relocate and reuse the space under it.  One-shot.  See
 * uml/repair_pin.sh.
 */
static unsigned int repair_hold_ms;
module_param_named(raid56_repair_hold_ms, repair_hold_ms, uint, 0644);
MODULE_PARM_DESC(raid56_repair_hold_ms,
		 "Hold each repair this many ms before it writes (testing only)");
#else
/* Constants, not macros: see rmw_no_repair. */
static const bool no_repair_on_fault;
static const bool repair_keeps_record;
static const bool repair_ignores_freeze;
static const bool repair_no_pin;
#endif

/* Caller holds wib->repair_lock. */
static void raid56_repair_schedule(struct btrfs_wib *wib)
{
	unsigned long first = 0;

	lockdep_assert_held(&wib->repair_lock);
	if (!wib->repair_nr || wib->repair_stopped || wib->repair_paused)
		return;
	for (unsigned int i = 0; i < wib->repair_nr; i++)
		if (i == 0 || time_before(wib->repair_queue[i].due, first))
			first = wib->repair_queue[i].due;
	mod_delayed_work(system_unbound_wq, &wib->repair_work,
			 time_after(first, jiffies) ? first - jiffies : 0);
}

void btrfs_raid56_queue_repair(struct btrfs_fs_info *fs_info, u64 full_stripe_start,
			       unsigned int tries)
{
	struct btrfs_wib *wib = fs_info->wib;
	unsigned long delay;

	if (!wib || READ_ONCE(no_repair_on_fault))
		return;
	delay = msecs_to_jiffies((unsigned long)READ_ONCE(repair_delay_ms) << tries);

	spin_lock(&wib->repair_lock);
	if (wib->repair_stopped)
		goto out;
	for (unsigned int i = 0; i < wib->repair_nr; i++)
		if (wib->repair_queue[i].logical == full_stripe_start)
			goto out;
	if (wib->repair_nr == BTRFS_WIB_REPAIR_SLOTS) {
		atomic64_inc(&wib->stat_repair_dropped);
		goto out;
	}
	wib->repair_queue[wib->repair_nr++] = (struct btrfs_wib_repair_slot) {
		.logical = full_stripe_start,
		.due = jiffies + delay,
		.tries = tries,
	};
	atomic64_inc(&wib->stat_repair_queued);
	raid56_repair_schedule(wib);
out:
	spin_unlock(&wib->repair_lock);
}

static void raid56_submit_repair(struct btrfs_fs_info *fs_info, u64 logical,
				 unsigned int tries)
{
	struct btrfs_wib *wib = fs_info->wib;
	struct btrfs_io_context *bioc = NULL;
	struct btrfs_block_group *bg;
	struct btrfs_raid_bio *rbio;
	u64 length = BTRFS_STRIPE_LEN;
	/* Read once: the control below must count and uncount consistently. */
	const bool unpinned = READ_ONCE(repair_no_pin);
	bool counted = false;
	bool busy;
	int ret;

	if (sb_rdonly(fs_info->sb) || btrfs_fs_closing(fs_info) ||
	    unlikely(BTRFS_FS_ERROR(fs_info)))
		return;
	/*
	 * In flight from here, BEFORE the block group is looked at: a
	 * relocation marks the group read-only and then drains
	 * (btrfs_raid56_drain_repairs()), and a repair counted only after
	 * passing the read-only test could slip between the two.
	 */
	if (!unpinned) {
		atomic_inc(&wib->repairs_inflight);
		counted = true;
	}
	/*
	 * A block group that is read-only belongs to a scrub, a balance or a
	 * replace, each of which rewrites this stripe its own way; leave it to
	 * them.  Gone altogether, or being removed, means nothing is left to
	 * repair.  Otherwise freeze it for the repair's lifetime, as scrub
	 * does: the cleaner may still remove an emptied group, but its chunk
	 * map -- and so its device space -- stays until the unfreeze, so the
	 * repair can never write into space already handed to someone else.
	 */
	bg = btrfs_lookup_block_group(fs_info, logical);
	if (!bg)
		goto out_inflight;
	spin_lock(&bg->lock);
	busy = bg->ro || !(bg->flags & BTRFS_BLOCK_GROUP_RAID56_MASK) ||
	       test_bit(BLOCK_GROUP_FLAG_REMOVED, &bg->runtime_flags);
	if (!busy && !unpinned)
		btrfs_freeze_block_group(bg);
	spin_unlock(&bg->lock);
	if (busy) {
		btrfs_put_block_group(bg);
		atomic64_inc(&wib->stat_repair_skipped);
		goto out_inflight;
	}
	if (unpinned) {
		/* The control: counted late, nothing frozen. */
		btrfs_put_block_group(bg);
		bg = NULL;
		atomic_inc(&wib->repairs_inflight);
		counted = true;
	}

	btrfs_bio_counter_inc_blocked(fs_info);
	ret = btrfs_map_block(fs_info, BTRFS_MAP_WRITE, logical, &length, &bioc,
			      NULL, NULL);
	if (ret < 0 || !bioc)
		goto out_counter;
	if (!(bioc->map_type & BTRFS_BLOCK_GROUP_RAID56_MASK) ||
	    bioc->full_stripe_logical != logical) {
		btrfs_put_bioc(bioc);
		goto out_counter;
	}
	rbio = alloc_rbio(fs_info, bioc);
	btrfs_put_bioc(bioc);
	if (IS_ERR(rbio))
		goto out_counter;
	rbio->operation = BTRFS_RBIO_WRITE;
	rbio->repair_tries = tries;
	rbio->repair_bg = bg;
	set_bit(RBIO_REPAIR_BIT, &rbio->flags);
	/*
	 * The in-flight count, the bio counter and the frozen block group are
	 * all dropped in raid56_repair_finished().
	 */
	start_async_work(rbio, rmw_rbio_work);
	return;

out_counter:
	btrfs_bio_counter_dec(fs_info);
	if (bg) {
		btrfs_unfreeze_block_group(bg);
		btrfs_put_block_group(bg);
	}
out_inflight:
	/*
	 * Only a count this call took.  The control leaves through here
	 * before it has counted itself -- for instance for every queued
	 * stripe of a chunk that has just been removed -- and dropping a count
	 * it never took would leave the total below zero, and unmount waiting
	 * in btrfs_raid56_stop_repairs() for a zero it can never reach.
	 */
	if (counted && atomic_dec_and_test(&wib->repairs_inflight))
		wake_up_all(&wib->wait);
}

void btrfs_raid56_repair_work(struct work_struct *work)
{
	struct btrfs_wib *wib = container_of(to_delayed_work(work),
					     struct btrfs_wib, repair_work);

	for (;;) {
		struct btrfs_wib_repair_slot slot;
		unsigned int i;

		spin_lock(&wib->repair_lock);
		if (wib->repair_stopped || wib->repair_paused) {
			spin_unlock(&wib->repair_lock);
			return;
		}
		for (i = 0; i < wib->repair_nr; i++)
			if (time_after_eq(jiffies, wib->repair_queue[i].due))
				break;
		if (i == wib->repair_nr) {
			raid56_repair_schedule(wib);
			spin_unlock(&wib->repair_lock);
			return;
		}
		slot = wib->repair_queue[i];
		wib->repair_queue[i] = wib->repair_queue[--wib->repair_nr];
		spin_unlock(&wib->repair_lock);

		raid56_submit_repair(wib->fs_info, slot.logical, slot.tries);
	}
}

static void raid56_repair_finished(struct btrfs_raid_bio *rbio, int ret, bool faulted)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 logical = rbio->bioc->full_stripe_logical;

	if (test_bit(RBIO_STALE_AMBIGUOUS_BIT, &rbio->flags)) {
		/* No retry changes that; it waits for a scrub or a human. */
		atomic64_inc(&wib->stat_repair_failed);
		btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_UNDECIDABLE, logical,
				   rbio->bioc, rbio_stale_cols(rbio));
	} else if (ret < 0 || faulted) {
		atomic64_inc(&wib->stat_repair_failed);
		if (rbio->repair_tries + 1 < RAID56_REPAIR_MAX_TRIES) {
			btrfs_raid56_queue_repair(fs_info, logical, rbio->repair_tries + 1);
		} else {
			btrfs_warn_rl(fs_info,
"raid56: gave up repairing full stripe %llu after %u attempts; it stays recorded for the next scrub or mount",
				      logical, RAID56_REPAIR_MAX_TRIES);
			btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_GAVE_UP, logical,
					   rbio->bioc, rbio_failed_cols(rbio));
		}
	} else {
		atomic64_inc(&wib->stat_repair_ok);
		/*
		 * Retire the record once nothing it names is left.  Every
		 * member it named stale has been written back or verified, and
		 * the parity rewritten wherever it was not trusted, so it no
		 * longer describes anything -- and left in place it would hold
		 * a slot until the next scrub or mount.  The log holds 165
		 * regions (82 while anything is stale); once full, every
		 * recorded write waits 60 seconds and fails.  Still under the
		 * stripe lock, so no write can land between the check and the
		 * retirement.
		 */
		if (!READ_ONCE(repair_keeps_record) && !rbio_stripe_recorded(rbio))
			btrfs_wib_clear_sticky(fs_info, logical,
					       (u64)rbio->nr_data << BTRFS_STRIPE_LEN_SHIFT);
	}
	btrfs_bio_counter_dec(fs_info);
	if (rbio->repair_bg) {
		btrfs_unfreeze_block_group(rbio->repair_bg);
		btrfs_put_block_group(rbio->repair_bg);
		rbio->repair_bg = NULL;
	}
	if (atomic_dec_and_test(&wib->repairs_inflight))
		wake_up_all(&wib->wait);
}

/*
 * Wait until no repair is in flight.  For relocation, after its block group
 * is read-only: a repair counts itself in flight before it looks at the
 * group, so every repair either saw the group read-only and skipped it, or is
 * waited for here.
 */
void btrfs_raid56_drain_repairs(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;

	if (wib && !READ_ONCE(repair_no_pin))
		wait_event(wib->wait, atomic_read(&wib->repairs_inflight) == 0);
}

/*
 * Freeze: keep the queue, submit nothing more, and let what is in flight
 * finish, so nothing is written to a frozen filesystem.  A repair in flight
 * never waits on anything freeze holds -- it takes no transaction and no
 * sb_start_write() -- so waiting for it here cannot deadlock.
 */
void btrfs_raid56_pause_repairs(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;

	if (!wib || READ_ONCE(repair_ignores_freeze))
		return;
	spin_lock(&wib->repair_lock);
	wib->repair_paused = true;
	spin_unlock(&wib->repair_lock);
	cancel_delayed_work_sync(&wib->repair_work);
	wait_event(wib->wait, atomic_read(&wib->repairs_inflight) == 0);
}

void btrfs_raid56_resume_repairs(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;

	if (!wib)
		return;
	spin_lock(&wib->repair_lock);
	wib->repair_paused = false;
	raid56_repair_schedule(wib);
	spin_unlock(&wib->repair_lock);
}

/*
 * No new repairs, none waiting, none in flight.  Before the filesystem stops
 * being writable (unmount, remount read-only): a repair writes.
 */
void btrfs_raid56_stop_repairs(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;

	if (!wib)
		return;
	spin_lock(&wib->repair_lock);
	wib->repair_stopped = true;
	/* Still recorded: the next scrub or mount repairs them. */
	wib->repair_nr = 0;
	spin_unlock(&wib->repair_lock);
	cancel_delayed_work_sync(&wib->repair_work);
	wait_event(wib->wait, atomic_read(&wib->repairs_inflight) == 0);
}

void btrfs_raid56_start_repairs(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;

	if (!wib)
		return;
	spin_lock(&wib->repair_lock);
	wib->repair_stopped = false;
	spin_unlock(&wib->repair_lock);
}

/*
 * Put the rebuilt sectors back BEFORE this write changes anything else, and
 * make that durable, record included.
 *
 * Written in the same batch as the new data and parity, a crash between them
 * could land the parity and not the write-back: the column is still stale on
 * disk, the record still names it, and the parity it is named against has
 * just changed -- mount recovery would rebuild it from a torn stripe and write
 * the result back, silently for a sector with no checksum.  So:
 *
 *   A: the rebuilt sectors (the whole named column, including sectors this
 *      write supplies, at the value the parity still describes), with FUA;
 *   then the column's mark cleared and the log made durable, flushed;
 *   B: the new data and parity, as any read-modify-write.
 *
 * If A does not land, or the log cannot be made durable, the write fails
 * before B: the parity is untouched and still holds the acknowledged value,
 * and the record still says where.
 */
static bool rmw_single_phase;
#ifdef CONFIG_BTRFS_DEBUG
module_param_named(raid56_rmw_single_phase, rmw_single_phase, bool, 0644);
MODULE_PARM_DESC(raid56_rmw_single_phase,
		 "Write repaired sectors in the same batch as the new parity (testing only: restores a known defect)");
#endif

static int rmw_repair_first(struct btrfs_raid_bio *rbio, u64 full_stripe_start)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const int data_sectors = rbio->nr_data * rbio->stripe_nsectors;
	struct bio_list bio_list;
	struct bio *bio;
	u64 cols = 0;
	int bit;
	int ret;

	bio_list_init(&bio_list);
	for_each_set_bit(bit, rbio->repair_bitmap, data_sectors) {
		const int stripe = bit / rbio->stripe_nsectors;
		const int sectornr = bit % rbio->stripe_nsectors;

		ret = rbio_add_io_paddrs(rbio, &bio_list,
					 rbio_stripe_paddrs(rbio, stripe, sectornr),
					 stripe, sectornr, REQ_OP_WRITE);
		if (ret) {
			bio_list_put(&bio_list);
			return ret;
		}
		cols |= BIT_ULL(stripe);
	}
	if (bio_list_empty(&bio_list))
		return 0;
	bio_list_for_each(bio, &bio_list)
		bio->bi_opf |= REQ_FUA;
	submit_write_bios(rbio, &bio_list);
	wait_event(rbio->io_wait, atomic_read(&rbio->stripes_pending) == 0);

	if (find_first_bit(rbio->error_bitmap, data_sectors) < data_sectors) {
		btrfs_warn_rl(fs_info,
"raid56: could not write back the rebuilt sectors of full stripe %llu; refusing the write rather than change a parity they still depend on",
			      full_stripe_start);
		/*
		 * A queued repair that could not write is not a refused write:
		 * it retries, and only giving up is news (raid56_repair_finished()).
		 */
		if (!test_bit(RBIO_REPAIR_BIT, &rbio->flags))
			btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_REFUSED,
					   full_stripe_start, rbio->bioc,
					   rbio_failed_cols(rbio));
		return -EIO;
	}

	/* Each such column matches the parity again; say so, durably. */
	for (int stripe = 0; stripe < rbio->nr_data; stripe++) {
		const int first = stripe * rbio->stripe_nsectors;
		bool all = true;

		if (!(cols & BIT_ULL(stripe)))
			continue;
		for (int nr = 0; nr < rbio->stripe_nsectors; nr++)
			if (!test_bit(first + nr, rbio->repair_bitmap) &&
			    !(rbio->verified_bitmap &&
			      test_bit(first + nr, rbio->verified_bitmap)))
				all = false;
		if (all)
			btrfs_wib_clear_stale(fs_info,
					      full_stripe_start + btrfs_stripe_nr_to_offset(stripe),
					      BTRFS_STRIPE_LEN);
	}
	ret = btrfs_wib_persist_now(fs_info);
	if (ret < 0) {
		btrfs_warn_rl(fs_info,
"raid56: could not make the repair of full stripe %llu durable (%d); refusing the write",
			      full_stripe_start, ret);
		if (!test_bit(RBIO_REPAIR_BIT, &rbio->flags))
			btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_NOT_DURABLE,
					   full_stripe_start, NULL, 0);
		return -EIO;
	}
	/* On disk now; phase B writes only the new data and the parity. */
	bitmap_clear(rbio->repair_bitmap, 0, data_sectors);
	return 0;
}

static void rmw_rbio(struct btrfs_raid_bio *rbio)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u64 full_stripe_start = rbio->bioc->full_stripe_logical;
	const u64 full_stripe_len = (u64)rbio->nr_data << BTRFS_STRIPE_LEN_SHIFT;
	struct bio_list bio_list;
	bool logged = false;
	bool faulted = false;
	/* Refused, and already reported as such. */
	bool refused = false;
	int crash_point = 0;
	int sectornr;
	int ret = 0;

	/*
	 * Allocate the pages for parity first, as P/Q pages will always be
	 * needed for both full-stripe and sub-stripe writes.
	 */
	ret = alloc_rbio_parity_pages(rbio);
	if (ret < 0)
		goto out;

	/*
	 * Either full stripe write, or we have every data sector already
	 * cached, can go to write path immediately.
	 */
	/*
	 * A repair has to look at the disks, not at a cached copy of what the
	 * filesystem believes is there: what is different between the two is
	 * the thing it is repairing.
	 *
	 * So does any sub-stripe write into a stripe the record names.  The
	 * cached pages hold the believed content, which makes the parity this
	 * write computes right -- but the write never reads, so it neither
	 * writes the stale column back (rmw_prepare_repair()) nor refuses an
	 * undecidable stripe, and if its own parity write then fails, the
	 * stale column and the parity are both wrong in the same vertical
	 * stripe: the acknowledged value is gone.  The model found this
	 * (raid56_redundancy_model.py, the kernel's cache rule: 76 lost states
	 * at nr_data 3 on a healthy RAID5).
	 */
	if (test_bit(RBIO_REPAIR_BIT, &rbio->flags) ||
	    (!rbio_is_full(rbio) &&
	     (need_read_stripe_sectors(rbio) ||
	      (!READ_ONCE(rmw_trust_cache) && rbio_stripe_recorded(rbio))))) {
		/*
		 * Now we're doing sub-stripe write, also need all data stripes
		 * to do the full RMW.
		 */
		ret = alloc_rbio_data_pages(rbio);
		if (ret < 0)
			goto out;

		index_rbio_pages(rbio);

		ret = rmw_read_wait_recover(rbio);
		if (ret < 0)
			goto out;

		/*
		 * The record names more of this stripe than its parity can
		 * rebuild.  The parity this write would compute has to take
		 * the stale column as it is on disk, which destroys the only
		 * copy of what was acknowledged there -- and for a column with
		 * no checksum, silently.  Refuse the write instead: the caller
		 * gets an error it can see, and the stripe keeps everything it
		 * still has, which may be enough once a missing device is back.
		 */
		if (test_bit(RBIO_STALE_AMBIGUOUS_BIT, &rbio->flags) &&
		    !READ_ONCE(rmw_no_refuse)) {
			btrfs_warn_rl(fs_info,
"raid56: refusing a write into full stripe %llu: the write-intent log records more of it stale than its parity can rebuild, and writing would destroy the only copy of acknowledged data",
				      full_stripe_start);
			atomic64_inc(&fs_info->raid56_write_stats.rmw_refused);
			/* A repair's is reported by raid56_repair_finished(). */
			if (!test_bit(RBIO_REPAIR_BIT, &rbio->flags))
				btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_UNDECIDABLE,
						   full_stripe_start, rbio->bioc,
						   rbio_stale_cols(rbio));
			refused = true;
			ret = -EIO;
			goto out;
		}
		rmw_prepare_repair(rbio);

		/* A repair that found nothing to put back writes nothing. */
		if (test_bit(RBIO_REPAIR_BIT, &rbio->flags) &&
		    bitmap_empty(&rbio->dbitmap, rbio->stripe_nsectors))
			goto out;
	}

	/*
	 * At this stage we're not allowed to add any new bios to the
	 * bio list any more, anyone else that wants to change this stripe
	 * needs to do their own rmw.
	 */
	spin_lock(&rbio->bio_list_lock);
	set_bit(RBIO_RMW_LOCKED_BIT, &rbio->flags);
	spin_unlock(&rbio->bio_list_lock);

	/*
	 * A sub-stripe write leaves the full stripe inconsistent if we crash
	 * in the middle of its writes, and other, committed data in the same
	 * vertical stripes would then lose its redundancy.  Record the full
	 * stripe in the write-intent log before submitting anything, so that
	 * the parity can be regenerated at the next mount.
	 *
	 * Full stripe COW writes only touch freshly allocated space and need
	 * no record: every sector of the stripe is unreferenced until the
	 * transaction referencing it commits, which happens only after all
	 * these writes have completed and been flushed.  In-place writes
	 * (nodatacow, prealloc) overwrite referenced sectors and are recorded
	 * even when they cover the full stripe.
	 */
	if (!rbio_is_full(rbio) || test_bit(RBIO_INPLACE_BIT, &rbio->flags)) {
		ret = btrfs_wib_mark(fs_info, full_stripe_start, full_stripe_len);
		if (ret < 0)
			goto out;
		logged = true;
#ifdef CONFIG_BTRFS_DEBUG
		if (unlikely(READ_ONCE(btrfs_raid56_crash_point) >= 1 &&
			     READ_ONCE(btrfs_raid56_crash_point) <= 3)) {
			crash_point = xchg(&btrfs_raid56_crash_point, 0);
			if (crash_point)
				btrfs_crit(fs_info,
			"raid56 crash injection %d armed for full stripe %llu",
					   crash_point, full_stripe_start);
		}
#endif
	}

#ifdef CONFIG_BTRFS_DEBUG
	/*
	 * Testing: hold ONE repair here, after it has read the stripe, decided
	 * what to write and marked the log, and before it writes.  One: every
	 * held rbio keeps an rmw worker asleep, and holding them all would
	 * stall every other write on the pool, which is a different test.
	 */
	if (unlikely(READ_ONCE(repair_hold_ms)) &&
	    test_bit(RBIO_REPAIR_BIT, &rbio->flags)) {
		const unsigned int hold = xchg(&repair_hold_ms, 0);

		if (hold) {
			btrfs_info(fs_info, "raid56: repair of full stripe %llu holding for %u ms",
				   full_stripe_start, hold);
			msleep(hold);
		}
	}
#endif

	bitmap_clear(rbio->error_bitmap, 0, rbio->nr_sectors);

	if (!READ_ONCE(rmw_single_phase)) {
		ret = rmw_repair_first(rbio, full_stripe_start);
		if (ret < 0) {
			refused = true;
			goto out;
		}
	}

	index_rbio_pages(rbio);

	/*
	 * We don't cache full rbios because we're assuming
	 * the higher layers are unlikely to use this area of
	 * the disk again soon.  If they do use it again,
	 * hopefully they will send another full bio.
	 */
	if (!rbio_is_full(rbio))
		cache_rbio_pages(rbio);
	else
		clear_bit(RBIO_CACHE_READY_BIT, &rbio->flags);

	for (sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++)
		generate_pq_vertical(rbio, sectornr);

	bio_list_init(&bio_list);
	ret = rmw_assemble_write_bios(rbio, &bio_list, crash_point);
	if (ret < 0)
		goto out;

	/* We should have at least one bio assembled. */
	ASSERT(bio_list_size(&bio_list));
	submit_write_bios(rbio, &bio_list);
	wait_event(rbio->io_wait, atomic_read(&rbio->stripes_pending) == 0);

#ifdef CONFIG_BTRFS_DEBUG
	if (unlikely(crash_point == 1 || crash_point == 2))
		panic("btrfs: raid56 crash injection %d at full stripe %llu",
		      crash_point, full_stripe_start);
#endif

	/*
	 * A write failure within the tolerance still costs the vertical stripe
	 * its redundancy, so it is worth one retry before accepting it.
	 */
	rmw_retry_failed_sectors(rbio);

	/* We may have more errors than our tolerance during the read. */
	for (sectornr = 0; sectornr < rbio->stripe_nsectors; sectornr++) {
		if (unlikely(get_rbio_vertical_faults(rbio, sectornr) >
			     rbio_max_errors(rbio))) {
			ret = -EIO;
			break;
		}
	}
out:
	/*
	 * The caller is told this write failed.  The stripe pages hold data
	 * that is not on disk, so they must not seed a later RMW through the
	 * stripe cache (cache_rbio()) or the plug list hand-off
	 * (steal_rbio()): that would compute the parity of the full stripe
	 * from content the filesystem does not have, and a reconstruction of
	 * a neighbouring committed sector would then return it.
	 *
	 * Only for a failed write.  Within the tolerance (ret == 0) the
	 * cached content is what the filesystem was told is on disk and the
	 * parity matches it.  Keeping it is safe because the next RMW into this
	 * stripe does not use it while the record names the stripe: it reads
	 * the disks, and so repairs or refuses (see the read decision above).
	 */
	if (ret < 0)
		clear_bit(RBIO_CACHE_READY_BIT, &rbio->flags);

	/*
	 * All writes of this RMW have completed (or none were submitted), so
	 * the stripe can leave the in-flight set.  The on-disk log is updated
	 * lazily with a flush, see raid56-wib.c.
	 *
	 * If any write failed (device error or missing device) the stripe is
	 * inconsistent on that device even without a crash; it stays logged
	 * so that the parity is regenerated at the next mount.
	 */
	if (logged) {
		const bool failed = ret < 0 ||
			!bitmap_empty(rbio->error_bitmap, rbio->nr_sectors);

		btrfs_wib_done(fs_info, full_stripe_start, full_stripe_len, failed);
		faulted = failed;
		/*
		 * Say WHICH data this write did and did not get onto the disk,
		 * not just that something went wrong.  A data stripe whose
		 * write failed holds its old content while the parity holds
		 * what the caller was told is there; the read path has to know
		 * that, or the next RMW of this full stripe will read the stale
		 * sector, believe it for want of a checksum, and fold it into
		 * the parity.  One log block is one BTRFS_STRIPE_LEN, so a data
		 * stripe is exactly one block of the recorded range.
		 *
		 * Unconditionally, and in particular whether or not the write
		 * as a whole was acknowledged.  It is tempting to record this
		 * only for a write the caller was told succeeded -- a refused
		 * write acknowledged nothing, so calling a column stale there
		 * looks like claiming proof we do not have.  That is the wrong
		 * layer.  These fields state what the DEVICES did; whether that
		 * amounts to proof is judged later, by the repair policy, which
		 * rebuilds a named column only when the columns it cannot
		 * believe are no more numerous than the parities it can still
		 * use.
		 *
		 * Recording only acknowledged writes breaks that judgement in
		 * both directions, and the model says so
		 * (tools/testing/btrfs/scrub_policy_model.py --policy
		 * prove-or-preserve --only-claim-on-acked): 480 misrepairs and
		 * 297 destroyed stripes at RAID5 depth 3, against none for
		 * recording unconditionally.  A refused write whose parity
		 * landed leaves that parity describing a vector nobody
		 * committed, and unless the record says so the budget believes
		 * the parity is a usable source and rebuilds live data out of
		 * it.
		 *
		 * The clearing half matters just as much: a write that lands on
		 * a column previously recorded stale is what makes it current
		 * again, and skipping that for a refused write leaves a stale
		 * mark on the one column that is now right.
		 */
		rmw_update_stale_data(rbio, full_stripe_start);
		rmw_update_stale_parity(rbio, full_stripe_start);
	}
	else if (ret >= 0 && !bitmap_empty(rbio->error_bitmap, rbio->nr_sectors)) {
		/*
		 * A full stripe write that a device did not take (within the
		 * tolerance): that device holds a stale sector of what is
		 * about to be committed, record the stripe for the next
		 * mount's scrub like a failed RMW.
		 *
		 * And say WHICH device, which this branch used to throw away.
		 * error_bitmap is right here and names the column and the
		 * sectors; recording only "something went wrong in this
		 * stripe" manufactures an unknown out of something known, and
		 * a stripe the log cannot name is one the repair path must
		 * decline.  That mattered most for the data it could least
		 * afford: NODATACOW is forced to copy-on-write on RAID5/6, so
		 * every checksumless write here is a full-stripe COW write and
		 * took this branch -- the one class with no checksum to appeal
		 * to was the one class that could never get a column name.
		 *
		 * Order matters: btrfs_wib_mark_stale() only marks blocks the
		 * log already records, which btrfs_wib_add_sticky() has just
		 * done.
		 */
		btrfs_wib_add_sticky(fs_info, full_stripe_start, full_stripe_len);
		rmw_update_stale_data(rbio, full_stripe_start);
		rmw_update_stale_parity(rbio, full_stripe_start);
		faulted = true;
	}

	/*
	 * A device did not take part of this write, so the stripe has lost
	 * redundancy it will not get back until something rewrites what the
	 * record now names.  Ask for that now rather than whenever a scrub or
	 * the next write to this stripe happens to come along.
	 */
	if (test_bit(RBIO_REPAIR_BIT, &rbio->flags)) {
		raid56_repair_finished(rbio, ret, faulted);
	} else if (faulted) {
		const unsigned long cols = rbio_failed_cols(rbio);

		/*
		 * Tell someone: within the tolerance the write succeeded but
		 * the stripe lost its redundancy; beyond it the caller gets
		 * EIO.  A refusal has said so already, and names the cause.
		 */
		if (ret >= 0)
			btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_STALE,
					   full_stripe_start, rbio->bioc, cols);
		else if (!refused && cols)
			btrfs_raid56_alert(fs_info, BTRFS_RAID56_EV_FAILED,
					   full_stripe_start, rbio->bioc, cols);
		btrfs_raid56_queue_repair(fs_info, full_stripe_start, 0);
	}
	rbio_orig_end_io(rbio, errno_to_blk_status(ret));
}

static void rmw_rbio_work(struct work_struct *work)
{
	struct btrfs_raid_bio *rbio;

	rbio = container_of(work, struct btrfs_raid_bio, work);
	if (lock_stripe_add(rbio) == 0)
		rmw_rbio(rbio);
}

static void rmw_rbio_work_locked(struct work_struct *work)
{
	rmw_rbio(container_of(work, struct btrfs_raid_bio, work));
}

/*
 * The following code is used to scrub/replace the parity stripe
 *
 * Caller must have already increased bio_counter for getting @bioc.
 *
 * Note: We need make sure all the pages that add into the scrub/replace
 * raid bio are correct and not be changed during the scrub/replace. That
 * is those pages just hold metadata or file data with checksum.
 */

struct btrfs_raid_bio *raid56_parity_alloc_scrub_rbio(struct bio *bio,
				struct btrfs_io_context *bioc,
				struct btrfs_device *scrub_dev,
				unsigned long *dbitmap, int stripe_nsectors)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	struct btrfs_raid_bio *rbio;
	int i;

	rbio = alloc_rbio(fs_info, bioc);
	if (IS_ERR(rbio))
		return NULL;
	bio_list_add(&rbio->bio_list, bio);
	/*
	 * This is a special bio which is used to hold the completion handler
	 * and make the scrub rbio is similar to the other types
	 */
	ASSERT(!bio->bi_iter.bi_size);
	rbio->operation = BTRFS_RBIO_PARITY_SCRUB;

	/*
	 * After mapping bioc with BTRFS_MAP_WRITE, parities have been sorted
	 * to the end position, so this search can start from the first parity
	 * stripe.
	 */
	for (i = rbio->nr_data; i < rbio->real_stripes; i++) {
		if (bioc->stripes[i].dev == scrub_dev) {
			rbio->scrubp = i;
			break;
		}
	}
	ASSERT_RBIO_STRIPE(i < rbio->real_stripes, rbio, i);

	bitmap_copy(&rbio->dbitmap, dbitmap, stripe_nsectors);
	return rbio;
}

static int alloc_rbio_sector_pages(struct btrfs_raid_bio *rbio,
				  int sector_nr)
{
	const u32 step = min(PAGE_SIZE, rbio->bioc->fs_info->sectorsize);
	const u32 base = sector_nr * rbio->sector_nsteps;

	for (int i = base; i < base + rbio->sector_nsteps; i++) {
		const unsigned int page_index = (i * step) >> PAGE_SHIFT;
		struct page *page;

		if (rbio->stripe_pages[page_index])
			continue;
		page = alloc_page(GFP_NOFS);
		if (!page)
			return -ENOMEM;
		rbio->stripe_pages[page_index] = page;
	}
	return 0;
}

/*
 * We just scrub the parity that we have correct data on the same horizontal,
 * so we needn't allocate all pages for all the stripes.
 */
static int alloc_rbio_essential_pages(struct btrfs_raid_bio *rbio)
{
	int total_sector_nr;

	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		int sectornr = total_sector_nr % rbio->stripe_nsectors;
		int ret;

		if (!test_bit(sectornr, &rbio->dbitmap))
			continue;
		ret = alloc_rbio_sector_pages(rbio, total_sector_nr);
		if (ret < 0)
			return ret;
	}
	index_stripe_sectors(rbio);
	return 0;
}

/* Return true if the content of the step matches the caclulated one. */
static bool verify_one_parity_step(struct btrfs_raid_bio *rbio,
				   void *pointers[], unsigned int sector_nr,
				   unsigned int step_nr)
{
	const unsigned int nr_data = rbio->nr_data;
	const bool has_qstripe = (rbio->real_stripes - rbio->nr_data == 2);
	const u32 step = min(rbio->bioc->fs_info->sectorsize, PAGE_SIZE);
	void *parity;
	bool ret = false;

	ASSERT(step_nr < rbio->sector_nsteps);

	/* First collect one page from each data stripe. */
	for (int stripe = 0; stripe < nr_data; stripe++)
		pointers[stripe] = kmap_local_paddr(
				sector_paddr_in_rbio(rbio, stripe, sector_nr,
						     step_nr, 0));

	if (has_qstripe) {
		assert_rbio(rbio);
		/* RAID6, call the library function to fill in our P/Q. */
		raid6_gen_syndrome(rbio->real_stripes, step, pointers);
	} else {
		/* RAID5. */
		memcpy(pointers[nr_data], pointers[0], step);
		xor_gen(pointers[nr_data], pointers + 1, nr_data - 1, step);
	}

	/* Check scrubbing parity and repair it. */
	parity = kmap_local_paddr(rbio_stripe_paddr(rbio, rbio->scrubp, sector_nr, step_nr));
	if (memcmp(parity, pointers[rbio->scrubp], step) != 0)
		memcpy(parity, pointers[rbio->scrubp], step);
	else
		ret = true;
	kunmap_local(parity);

	for (int stripe = nr_data - 1; stripe >= 0; stripe--)
		kunmap_local(pointers[stripe]);
	return ret;
}

/*
 * The @pointers array should have the P/Q parity already mapped.
 */
static void verify_one_parity_sector(struct btrfs_raid_bio *rbio,
				     void *pointers[], unsigned int sector_nr)
{
	bool found_error = false;

	for (int step_nr = 0; step_nr < rbio->sector_nsteps; step_nr++) {
		bool match;

		match = verify_one_parity_step(rbio, pointers, sector_nr, step_nr);
		if (!match)
			found_error = true;
	}
	if (!found_error)
		bitmap_clear(&rbio->dbitmap, sector_nr, 1);
}

static int finish_parity_scrub(struct btrfs_raid_bio *rbio)
{
	struct btrfs_io_context *bioc = rbio->bioc;
	void **pointers = rbio->finish_pointers;
	unsigned long *pbitmap = &rbio->finish_pbitmap;
	unsigned int nr_mismatch;
	int nr_data = rbio->nr_data;
	int sectornr;
	bool has_qstripe;
	struct page *page;
	phys_addr_t p_paddr = INVALID_PADDR;
	phys_addr_t q_paddr = INVALID_PADDR;
	struct bio_list bio_list;
	bool is_replace = false;
	int ret;

	bio_list_init(&bio_list);

	if (rbio->real_stripes - rbio->nr_data == 1)
		has_qstripe = false;
	else if (rbio->real_stripes - rbio->nr_data == 2)
		has_qstripe = true;
	else
		BUG();

	/*
	 * Replace is running and our P/Q stripe is being replaced, then we
	 * need to duplicate the final write to replace target.
	 */
	if (bioc->replace_nr_stripes && bioc->replace_stripe_src == rbio->scrubp) {
		is_replace = true;
		bitmap_copy(pbitmap, &rbio->dbitmap, rbio->stripe_nsectors);
	}

	/*
	 * Because the higher layers(scrubber) are unlikely to
	 * use this area of the disk again soon, so don't cache
	 * it.
	 */
	clear_bit(RBIO_CACHE_READY_BIT, &rbio->flags);

	page = alloc_page(GFP_NOFS);
	if (!page)
		return -ENOMEM;
	p_paddr = page_to_phys(page);
	page = NULL;
	pointers[nr_data] = kmap_local_paddr(p_paddr);

	if (has_qstripe) {
		/* RAID6, allocate and map temp space for the Q stripe */
		page = alloc_page(GFP_NOFS);
		if (!page) {
			__free_page(phys_to_page(p_paddr));
			p_paddr = INVALID_PADDR;
			return -ENOMEM;
		}
		q_paddr = page_to_phys(page);
		page = NULL;
		pointers[rbio->real_stripes - 1] = kmap_local_paddr(q_paddr);
	}

	bitmap_clear(rbio->error_bitmap, 0, rbio->nr_sectors);

	/* Map the parity stripe just once */

	for_each_set_bit(sectornr, &rbio->dbitmap, rbio->stripe_nsectors)
		verify_one_parity_sector(rbio, pointers, sectornr);

	/*
	 * Whatever is still set did not match: verify_one_parity_sector()
	 * clears the bit of every vertical stripe whose parity already
	 * described the data.  So this is the write hole's footprint, measured
	 * without needing any record to have been kept -- which is the only
	 * way to measure it on a filesystem damaged by an older kernel.  It
	 * was always known here and always corrected in silence.
	 *
	 * Say it out loud.  On an array that has never lost a device, a
	 * non-zero count means writes were lost, and names where.
	 */
	nr_mismatch = bitmap_weight(&rbio->dbitmap, rbio->stripe_nsectors);
	if (unlikely(nr_mismatch)) {
		struct btrfs_raid56_write_stats *st = &bioc->fs_info->raid56_write_stats;

		atomic64_add(nr_mismatch, &st->parity_mismatch);
		atomic64_inc(&st->parity_mismatch_stripes);
		btrfs_warn_rl(bioc->fs_info,
"raid56: parity of full stripe %llu did not describe its data in %u of %u vertical stripe(s); rewriting it. On an array that has not lost a device this is the write hole, and the data it disagreed with may be what was lost",
			      bioc->full_stripe_logical, nr_mismatch,
			      rbio->stripe_nsectors);
	}

	kunmap_local(pointers[nr_data]);
	__free_page(phys_to_page(p_paddr));
	p_paddr = INVALID_PADDR;
	if (q_paddr != INVALID_PADDR) {
		__free_page(phys_to_page(q_paddr));
		q_paddr = INVALID_PADDR;
	}

	/*
	 * time to start writing.  Make bios for everything from the
	 * higher layers (the bio_list in our rbio) and our p/q.  Ignore
	 * everything else.
	 */
	for_each_set_bit(sectornr, &rbio->dbitmap, rbio->stripe_nsectors) {
		phys_addr_t *paddrs;

		paddrs = rbio_stripe_paddrs(rbio, rbio->scrubp, sectornr);
		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, rbio->scrubp,
					 sectornr, REQ_OP_WRITE);
		if (ret)
			goto cleanup;
	}

	if (!is_replace)
		goto submit_write;

	/*
	 * Replace is running and our parity stripe needs to be duplicated to
	 * the target device.  Check we have a valid source stripe number.
	 */
	ASSERT_RBIO(rbio->bioc->replace_stripe_src >= 0, rbio);
	for_each_set_bit(sectornr, pbitmap, rbio->stripe_nsectors) {
		phys_addr_t *paddrs;

		paddrs = rbio_stripe_paddrs(rbio, rbio->scrubp, sectornr);
		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, rbio->real_stripes,
					 sectornr, REQ_OP_WRITE);
		if (ret)
			goto cleanup;
	}

submit_write:
	submit_write_bios(rbio, &bio_list);
	return 0;

cleanup:
	bio_list_put(&bio_list);
	return ret;
}

static inline int is_data_stripe(struct btrfs_raid_bio *rbio, int stripe)
{
	if (stripe >= 0 && stripe < rbio->nr_data)
		return 1;
	return 0;
}

static int recover_scrub_rbio(struct btrfs_raid_bio *rbio)
{
	void **pointers = NULL;
	void **unmap_array = NULL;
	int sector_nr;
	int ret = 0;

	/*
	 * @pointers array stores the pointer for each sector.
	 *
	 * @unmap_array stores copy of pointers that does not get reordered
	 * during reconstruction so that kunmap_local works.
	 */
	pointers = kzalloc_objs(void *, rbio->real_stripes, GFP_NOFS);
	unmap_array = kzalloc_objs(void *, rbio->real_stripes, GFP_NOFS);
	if (!pointers || !unmap_array) {
		ret = -ENOMEM;
		goto out;
	}

	for (sector_nr = 0; sector_nr < rbio->stripe_nsectors; sector_nr++) {
		int dfail = 0, failp = -1;
		int faila;
		int failb;
		int found_errors;

		found_errors = get_rbio_vertical_errors(rbio, sector_nr,
							 &faila, &failb);
		if (unlikely(found_errors > rbio_max_errors(rbio))) {
			ret = -EIO;
			goto out;
		}
		if (found_errors == 0)
			continue;

		/* We should have at least one error here. */
		ASSERT(faila >= 0 || failb >= 0);

		if (is_data_stripe(rbio, faila))
			dfail++;
		else if (is_parity_stripe(faila))
			failp = faila;

		if (is_data_stripe(rbio, failb))
			dfail++;
		else if (is_parity_stripe(failb))
			failp = failb;
		/*
		 * Because we can not use a scrubbing parity to repair the
		 * data, so the capability of the repair is declined.  (In the
		 * case of RAID5, we can not repair anything.)
		 */
		if (unlikely(dfail > rbio_max_errors(rbio) - 1)) {
			ret = -EIO;
			goto out;
		}
		/*
		 * If all data is good, only parity is correctly, just repair
		 * the parity, no need to recover data stripes.
		 */
		if (dfail == 0)
			continue;

		/*
		 * Here means we got one corrupted data stripe and one
		 * corrupted parity on RAID6, if the corrupted parity is
		 * scrubbing parity, luckily, use the other one to repair the
		 * data, or we can not repair the data stripe.
		 */
		if (unlikely(failp != rbio->scrubp)) {
			ret = -EIO;
			goto out;
		}

		ret = recover_vertical(rbio, sector_nr, pointers, unmap_array,
				       NULL, NULL);
		if (ret < 0)
			goto out;
	}
out:
	kfree(pointers);
	kfree(unmap_array);
	return ret;
}

static int scrub_assemble_read_bios(struct btrfs_raid_bio *rbio)
{
	struct bio_list bio_list = BIO_EMPTY_LIST;
	int total_sector_nr;
	int ret = 0;

	/* Build a list of bios to read all the missing parts. */
	for (total_sector_nr = 0; total_sector_nr < rbio->nr_sectors;
	     total_sector_nr++) {
		int sectornr = total_sector_nr % rbio->stripe_nsectors;
		int stripe = total_sector_nr / rbio->stripe_nsectors;
		phys_addr_t *paddrs;

		/* No data in the vertical stripe, no need to read. */
		if (!test_bit(sectornr, &rbio->dbitmap))
			continue;

		/*
		 * A parity-scrub rbio carries no data in its bio list: the
		 * only bio there is the empty completion bio added by
		 * raid56_parity_alloc_scrub_rbio().  Every sector is read
		 * from the stripe, so only assert that invariant here.
		 */
		ASSERT(!sector_paddrs_in_rbio(rbio, stripe, sectornr, 1));

		paddrs = rbio_stripe_paddrs(rbio, stripe, sectornr);
		/*
		 * The bio cache may have handed us an uptodate sector.  If so,
		 * use it.
		 */
		if (test_bit(rbio_sector_index(rbio, stripe, sectornr),
			     rbio->stripe_uptodate_bitmap))
			continue;

		ret = rbio_add_io_paddrs(rbio, &bio_list, paddrs, stripe,
					 sectornr, REQ_OP_READ);
		if (ret) {
			bio_list_put(&bio_list);
			return ret;
		}
	}

	submit_read_wait_bio_list(rbio, &bio_list);
	return 0;
}

static void scrub_rbio(struct btrfs_raid_bio *rbio)
{
	int sector_nr;
	int ret;

	ret = alloc_rbio_essential_pages(rbio);
	if (ret)
		goto out;

	bitmap_clear(rbio->error_bitmap, 0, rbio->nr_sectors);

	ret = scrub_assemble_read_bios(rbio);
	if (ret < 0)
		goto out;

	/* We may have some failures, recover the failed sectors first. */
	ret = recover_scrub_rbio(rbio);
	if (ret < 0)
		goto out;

	/*
	 * We have every sector properly prepared. Can finish the scrub
	 * and writeback the good content.
	 */
#ifdef CONFIG_BTRFS_DEBUG
	if (unlikely(READ_ONCE(btrfs_raid56_crash_point) == 4) &&
	    xchg(&btrfs_raid56_crash_point, 0) == 4)
		panic("btrfs: raid56 crash injection 4 before parity write of full stripe %llu",
		      rbio->bioc->full_stripe_logical);
#endif
	ret = finish_parity_scrub(rbio);
	wait_event(rbio->io_wait, atomic_read(&rbio->stripes_pending) == 0);
	for (sector_nr = 0; sector_nr < rbio->stripe_nsectors; sector_nr++) {
		int found_errors;

		found_errors = get_rbio_vertical_errors(rbio, sector_nr, NULL, NULL);
		if (unlikely(found_errors > rbio_max_errors(rbio))) {
			ret = -EIO;
			break;
		}
	}
	/*
	 * finish_parity_scrub() cleared the error bitmap before the writes,
	 * so anything set now is a failed parity write.
	 */
	if (ret == 0 && test_bit(RBIO_STRICT_WRITE_BIT, &rbio->flags) &&
	    !bitmap_empty(rbio->error_bitmap, rbio->nr_sectors))
		ret = -EREMOTEIO;
out:
	rbio_orig_end_io(rbio, errno_to_blk_status(ret));
}

/* See RBIO_STRICT_WRITE_BIT. */
void raid56_parity_scrub_rbio_strict(struct btrfs_raid_bio *rbio)
{
	set_bit(RBIO_STRICT_WRITE_BIT, &rbio->flags);
}

static void scrub_rbio_work_locked(struct work_struct *work)
{
	scrub_rbio(container_of(work, struct btrfs_raid_bio, work));
}

void raid56_parity_submit_scrub_rbio(struct btrfs_raid_bio *rbio)
{
	if (!lock_stripe_add(rbio))
		start_async_work(rbio, scrub_rbio_work_locked);
}

/*
 * This is for scrub call sites where we already have correct data contents.
 * This allows us to avoid reading data stripes again.
 *
 * Unfortunately here we have to do folio copy, other than reusing the pages.
 * This is due to the fact rbio has its own page management for its cache.
 */
void raid56_parity_cache_data_folios(struct btrfs_raid_bio *rbio,
				     void *vaddr, u64 data_logical)
{
	struct btrfs_fs_info *fs_info = rbio->bioc->fs_info;
	const u64 offset_in_full_stripe = data_logical -
					  rbio->bioc->full_stripe_logical;
	int ret;

	/*
	 * If we hit ENOMEM temporarily, but later at
	 * raid56_parity_submit_scrub_rbio() time it succeeded, we just do
	 * the extra read, not a big deal.
	 *
	 * If we hit ENOMEM later at raid56_parity_submit_scrub_rbio() time,
	 * the bio would got proper error number set.
	 */
	ret = alloc_rbio_data_pages(rbio);
	if (ret < 0)
		return;

	/* data_logical must be at stripe boundary and inside the full stripe. */
	ASSERT(IS_ALIGNED(offset_in_full_stripe, BTRFS_STRIPE_LEN));
	ASSERT(offset_in_full_stripe < (rbio->nr_data << BTRFS_STRIPE_LEN_SHIFT));

	for (unsigned int cur_off = offset_in_full_stripe;
	     cur_off < offset_in_full_stripe + BTRFS_STRIPE_LEN;
	     cur_off += PAGE_SIZE) {
		const unsigned int pindex = cur_off >> PAGE_SHIFT;

		ASSERT(cur_off - offset_in_full_stripe + PAGE_SIZE <= BTRFS_STRIPE_LEN);
		memcpy_to_page(rbio->stripe_pages[pindex], 0,
			       vaddr + cur_off - offset_in_full_stripe, PAGE_SIZE);
	}
	bitmap_set(rbio->stripe_uptodate_bitmap,
		   offset_in_full_stripe >> fs_info->sectorsize_bits,
		   BTRFS_STRIPE_LEN >> fs_info->sectorsize_bits);
}
