/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_SCRUB_H
#define BTRFS_SCRUB_H

#include <linux/types.h>

struct btrfs_fs_info;
struct btrfs_device;
struct btrfs_scrub_progress;

int btrfs_scrub_dev(struct btrfs_fs_info *fs_info, u64 devid, u64 start,
		    u64 end, struct btrfs_scrub_progress *progress,
		    bool readonly, bool is_dev_replace);
int btrfs_raid56_full_stripe_range(struct btrfs_fs_info *fs_info, u64 logical,
				   u64 *full_stripe_start, u64 *full_stripe_len);
struct scrub_ctx;
struct scrub_ctx *btrfs_scrub_raid56_recovery_begin(struct btrfs_fs_info *fs_info);
void btrfs_scrub_raid56_recovery_end(struct btrfs_fs_info *fs_info,
				     struct scrub_ctx *sctx);
/*
 * What btrfs_scrub_raid56_full_stripe() may do to a stripe the write-intent
 * log recorded.  These used to be one bool, @trusted, which meant two things
 * at once -- "the extent tree is complete" and "regenerate the parity over
 * sectors that cannot be verified" -- so error records could never be
 * repaired at all, only verified.
 */
enum btrfs_raid56_recover_mode {
	/* Verify and repair the data columns; write no parity. */
	BTRFS_RAID56_RECOVER_VERIFY,
	/*
	 * Decide exactly as a user scrub decides, using the record: rebuild a
	 * column it names, decline a stripe it cannot decide, regenerate the
	 * parity and retire the record only when the repair reached the disk.
	 * The record must be in the live table first; that is what the plan
	 * reads.
	 */
	BTRFS_RAID56_RECOVER_SCRUB,
	/*
	 * In-flight record only (a crash mid-RMW, no device reported anything):
	 * as a scrub, and then every vertical stripe's parity recomputed from
	 * the data on disk, which covers extents only the tree log knows.
	 */
	BTRFS_RAID56_RECOVER_TRUSTED,
};

int btrfs_scrub_raid56_full_stripe(struct btrfs_fs_info *fs_info,
				   struct scrub_ctx *sctx,
				   u64 full_stripe_start,
				   enum btrfs_raid56_recover_mode mode);
void btrfs_scrub_pause(struct btrfs_fs_info *fs_info);
void btrfs_scrub_continue(struct btrfs_fs_info *fs_info);
int btrfs_scrub_cancel(struct btrfs_fs_info *info);
int btrfs_scrub_cancel_dev(struct btrfs_device *dev);
int btrfs_scrub_progress(struct btrfs_fs_info *fs_info, u64 devid,
			 struct btrfs_scrub_progress *progress);

#endif
