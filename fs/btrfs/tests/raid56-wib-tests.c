// SPDX-License-Identifier: GPL-2.0
/*
 * Self tests for the RAID56 write-intent log (in-memory tracking, on-disk
 * block encoding, torn block detection, recovery list merging and the
 * enable/disable ordering).
 *
 * The dummy fs_info has no devices, so commits build the block but write it
 * nowhere; the block content is verified through wib->last.
 */

#include <linux/types.h>
#include "btrfs-tests.h"
#include "../ctree.h"
#include "../fs.h"
#include "../volumes.h"
#include "../accessors.h"
#include "../raid56.h"
#include "../raid56-wib.h"

static const u8 test_uuid[BTRFS_FSID_SIZE] = {
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
	0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
};

static int check_block_entry(const void *block, u32 index, u64 bytenr, u64 bitmap,
			     u64 error)
{
	const struct btrfs_wib_disk_header *hdr = block;
	struct btrfs_wib_entry e;

	if (le32_to_cpu(hdr->nr_entries) <= index) {
		test_err("block has %u entries, expected entry %u",
			 le32_to_cpu(hdr->nr_entries), index);
		return -EINVAL;
	}
	/*
	 * Decode rather than index: a block is written in the narrow layout
	 * whenever nothing is stale, so the stride is not a constant and
	 * indexing one struct over the other reads the wrong fields.
	 */
	btrfs_wib_read_entry(block, index, &e);
	if (e.bytenr != bytenr || e.bitmap != bitmap || e.sticky != error) {
		test_err("entry %u is (%llu, 0x%llx, 0x%llx), expected (%llu, 0x%llx, 0x%llx)",
			 index, e.bytenr, e.bitmap, e.sticky,
			 bytenr, bitmap, error);
		return -EINVAL;
	}
	return 0;
}

static u32 block_nr_entries(const void *block)
{
	return le32_to_cpu(((const struct btrfs_wib_disk_header *)block)->nr_entries);
}

static int test_range_mask(void)
{
	const u64 base = 3 * BTRFS_WIB_ENTRY_SIZE;

	/* Single 64K block at the start of the entry. */
	if (btrfs_wib_range_mask(base, base, BTRFS_WIB_BLOCK_SIZE) != 0x1) {
		test_err("range mask for first block wrong");
		return -EINVAL;
	}
	/* Last block of the entry. */
	if (btrfs_wib_range_mask(base, base + 63 * BTRFS_WIB_BLOCK_SIZE,
				 BTRFS_WIB_BLOCK_SIZE) != (1ULL << 63)) {
		test_err("range mask for last block wrong");
		return -EINVAL;
	}
	/* A 3 data stripe RAID5 full stripe (192K) straddling two entries. */
	if (btrfs_wib_range_mask(base, base + 62 * BTRFS_WIB_BLOCK_SIZE,
				 3 * BTRFS_WIB_BLOCK_SIZE) != (0x3ULL << 62)) {
		test_err("range mask for straddling stripe, first entry wrong");
		return -EINVAL;
	}
	if (btrfs_wib_range_mask(base + BTRFS_WIB_ENTRY_SIZE,
				 base + 62 * BTRFS_WIB_BLOCK_SIZE,
				 3 * BTRFS_WIB_BLOCK_SIZE) != 0x1) {
		test_err("range mask for straddling stripe, second entry wrong");
		return -EINVAL;
	}
	/* Disjoint range. */
	if (btrfs_wib_range_mask(base, base + BTRFS_WIB_ENTRY_SIZE, SZ_64K) != 0) {
		test_err("range mask for disjoint range not zero");
		return -EINVAL;
	}
	/* Whole entry. */
	if (btrfs_wib_range_mask(base, base, BTRFS_WIB_ENTRY_SIZE) != ~0ULL) {
		test_err("range mask for whole entry wrong");
		return -EINVAL;
	}
	return 0;
}

static int test_mark_commit_done(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const struct btrfs_wib_disk_header *hdr = wib->last;
	const u64 stripe_a = 5 * BTRFS_WIB_ENTRY_SIZE + 4 * BTRFS_WIB_BLOCK_SIZE;
	const u64 stripe_b = 9 * BTRFS_WIB_ENTRY_SIZE + 62 * BTRFS_WIB_BLOCK_SIZE;
	const u64 stripe_c = 12 * BTRFS_WIB_ENTRY_SIZE;
	void *saved;
	int ret;

	saved = kmalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!saved)
		return -ENOMEM;

	/* Not enabled: marks are tracked in memory only, no commit happens. */
	ret = btrfs_wib_mark(fs_info, stripe_a, 2 * BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("mark failed: %d", ret);
		goto out;
	}
	if (wib->seq != 0 || le64_to_cpu(hdr->magic) == BTRFS_WIB_MAGIC) {
		test_err("commit happened while the log was disabled");
		ret = -EINVAL;
		goto out;
	}

	/* Enabling persists what is in flight. */
	ret = btrfs_wib_enable(fs_info);
	if (ret) {
		test_err("enable failed: %d", ret);
		goto out;
	}
	if (!btrfs_wib_block_valid(fs_info, wib->last)) {
		test_err("committed block is not valid");
		ret = -EINVAL;
		goto out;
	}
	if (le64_to_cpu(hdr->seq) != 1 || wib->seq != 1) {
		test_err("first commit has seq %llu/%llu, expected 1",
			 le64_to_cpu(hdr->seq), wib->seq);
		ret = -EINVAL;
		goto out;
	}
	ret = check_block_entry(wib->last, 0, 5 * BTRFS_WIB_ENTRY_SIZE, 0x3ULL << 4, 0);
	if (ret)
		goto out;

	/* A stripe straddling two entries. */
	ret = btrfs_wib_mark(fs_info, stripe_b, 3 * BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("second mark failed: %d", ret);
		goto out;
	}
	if (block_nr_entries(wib->last) != 3 || wib->seq != 2) {
		test_err("after second mark: %u entries, seq %llu",
			 block_nr_entries(wib->last), wib->seq);
		ret = -EINVAL;
		goto out;
	}
	ret = check_block_entry(wib->last, 1, 9 * BTRFS_WIB_ENTRY_SIZE, 0x3ULL << 62, 0);
	if (ret)
		goto out;
	ret = check_block_entry(wib->last, 2, 10 * BTRFS_WIB_ENTRY_SIZE, 0x1, 0);
	if (ret)
		goto out;

	/* A mark that is already covered doesn't need a new commit. */
	ret = btrfs_wib_mark(fs_info, stripe_a, 2 * BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("re-mark failed: %d", ret);
		goto out;
	}
	if (le64_to_cpu(hdr->seq) != 2) {
		test_err("re-mark rewrote the block (seq %llu)", le64_to_cpu(hdr->seq));
		ret = -EINVAL;
		goto out;
	}

	/* An unaligned mark is widened to whole blocks. */
	ret = btrfs_wib_mark(fs_info, stripe_a + SZ_4K, SZ_4K);
	if (ret) {
		test_err("unaligned mark failed: %d", ret);
		goto out;
	}
	if (le64_to_cpu(hdr->seq) != 2) {
		test_err("unaligned mark inside a marked block rewrote the block");
		ret = -EINVAL;
		goto out;
	}

	/* Done clears in memory only; the committed block is untouched. */
	memcpy(saved, wib->last, BTRFS_WIB_SLOT_SIZE);
	btrfs_wib_done(fs_info, stripe_a, 2 * BTRFS_WIB_BLOCK_SIZE, false);
	if (block_nr_entries(wib->last) != 3) {
		test_err("done modified the committed block");
		ret = -EINVAL;
		goto out;
	}
	/*
	 * A mark-time commit only adds: the finished stripe stays listed
	 * (no flush happened that would allow dropping it).
	 */
	ret = btrfs_wib_mark(fs_info, stripe_c, BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("third mark failed: %d", ret);
		goto out;
	}
	if (block_nr_entries(wib->last) != 4 || btrfs_wib_block_drops(saved, wib->last)) {
		test_err("mark-time commit dropped a finished stripe (%u entries)",
			 block_nr_entries(wib->last));
		ret = -EINVAL;
		goto out;
	}
	ret = check_block_entry(wib->last, 3, 5 * BTRFS_WIB_ENTRY_SIZE, 0x3ULL << 4, 0);
	if (ret)
		goto out;
	btrfs_wib_done(fs_info, stripe_c, BTRFS_WIB_BLOCK_SIZE, false);

	/* The transaction commit, after a confirmed flush, drops it. */
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret) {
		test_err("commit failed: %d", ret);
		goto out;
	}
	if (block_nr_entries(wib->last) != 2) {
		test_err("commit after done kept %u entries", block_nr_entries(wib->last));
		ret = -EINVAL;
		goto out;
	}
	if (!btrfs_wib_block_drops(saved, wib->last)) {
		test_err("drop of a finished stripe not detected");
		ret = -EINVAL;
		goto out;
	}
	if (btrfs_wib_block_drops(wib->last, saved)) {
		test_err("false drop detected");
		ret = -EINVAL;
		goto out;
	}

	/* Nothing changed: commit is a no-op. */
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret) {
		test_err("no-op commit failed: %d", ret);
		goto out;
	}
	if (atomic64_read(&wib->stat_commits) != 4) {
		test_err("no-op commit did IO (%llu commits)",
			 (unsigned long long)atomic64_read(&wib->stat_commits));
		ret = -EINVAL;
		goto out;
	}

	/* But not if the last block is not known to have reached every device. */
	wib->last_ok = false;
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		goto out;
	if (atomic64_read(&wib->stat_commits) != 5 || !wib->last_ok) {
		test_err("commit after a failed one did no IO");
		ret = -EINVAL;
		goto out;
	}

	/*
	 * A device did not confirm the flush: the finished stripe must not
	 * be dropped, it becomes an error record.
	 */
	btrfs_wib_done(fs_info, stripe_b, 3 * BTRFS_WIB_BLOCK_SIZE, false);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, false);
	if (ret)
		goto out;
	if (block_nr_entries(wib->last) != 2) {
		test_err("commit without a confirmed flush dropped a stripe (%u entries)",
			 block_nr_entries(wib->last));
		ret = -EINVAL;
		goto out;
	}
	/* Still listed as in flight (union with the last block) and as an error record. */
	ret = check_block_entry(wib->last, 0, 9 * BTRFS_WIB_ENTRY_SIZE, 0x3ULL << 62,
				0x3ULL << 62);
	if (ret)
		goto out;
	ret = check_block_entry(wib->last, 1, 10 * BTRFS_WIB_ENTRY_SIZE, 0x1, 0x1);
	if (ret)
		goto out;
	btrfs_wib_clear_sticky(fs_info, stripe_b, 3 * BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		goto out;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after all stripes finished");
		ret = -EINVAL;
		goto out;
	}

	/*
	 * A stripe recorded and finished between the transaction commit's
	 * snapshot and its (flushed) log write may have written during the
	 * flush: it must stay listed.
	 */
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_mark(fs_info, stripe_c, BTRFS_WIB_BLOCK_SIZE);
	if (ret)
		goto out;
	btrfs_wib_done(fs_info, stripe_c, BTRFS_WIB_BLOCK_SIZE, false);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		goto out;
	if (block_nr_entries(wib->last) != 1) {
		test_err("stripe recorded after the snapshot dropped by the flushed commit");
		ret = -EINVAL;
		goto out;
	}
	ret = check_block_entry(wib->last, 0, stripe_c, 0x1, 0);
	if (ret)
		goto out;
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		goto out;
	if (block_nr_entries(wib->last) != 0) {
		test_err("finished stripe not dropped by the next flushed commit");
		ret = -EINVAL;
		goto out;
	}
	ret = 0;
out:
	kfree(saved);
	return ret;
}

/*
 * Re-stamp the checksum after doctoring a header field, so that a rejection
 * proves the field check fired rather than the checksum.
 */
static void restamp(struct btrfs_fs_info *fs_info, void *block)
{
	struct btrfs_wib_disk_header *hdr = block;

	btrfs_csum(fs_info->csum_type, block + BTRFS_CSUM_SIZE,
		   BTRFS_WIB_SLOT_SIZE - BTRFS_CSUM_SIZE, hdr->csum);
}

static int test_torn_block(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	struct btrfs_wib_disk_header *hdr;
	void *block;
	int ret = -EINVAL;

	block = kmalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!block)
		return -ENOMEM;
	hdr = block;

	btrfs_wib_build_block(wib, block, 42, NULL);
	if (!btrfs_wib_block_valid(fs_info, block)) {
		test_err("freshly built block not valid");
		goto out;
	}
	/* Torn write: the last byte of the slot, the marker's, changed. */
	((u8 *)block)[BTRFS_WIB_SLOT_SIZE - 1] ^= 0x5a;
	if (btrfs_wib_block_valid(fs_info, block)) {
		test_err("corrupted block accepted");
		goto out;
	}
	((u8 *)block)[BTRFS_WIB_SLOT_SIZE - 1] ^= 0x5a;

	/* Block of another filesystem. */
	hdr->fsid[0] ^= 1;
	if (btrfs_wib_block_valid(fs_info, block)) {
		test_err("block with foreign fsid accepted");
		goto out;
	}
	hdr->fsid[0] ^= 1;

	/* Bad magic. */
	hdr->magic = 0;
	if (btrfs_wib_block_valid(fs_info, block)) {
		test_err("block with bad magic accepted");
		goto out;
	}

	/* All zero (never written) block. */
	memset(block, 0, BTRFS_WIB_SLOT_SIZE);
	if (btrfs_wib_block_valid(fs_info, block)) {
		test_err("zeroed block accepted");
		goto out;
	}

	/*
	 * A block written by a kernel that understands more of the format than
	 * this one.  Each field is set and the checksum re-stamped, so that
	 * what rejects the block is the field check and not merely a checksum
	 * that no longer matches -- otherwise these would pass whether or not
	 * the checks exist.
	 */
	btrfs_wib_build_block(wib, block, 43, NULL);
	/*
	 * An UNKNOWN flag, i.e. one bit above everything this kernel defines.
	 * Not simply bit 0: that is BTRFS_WIB_FLAG_STALE now, and a test that
	 * sets a flag which has since been defined stops testing anything
	 * while still passing.
	 */
	hdr->flags = cpu_to_le64(BTRFS_WIB_FLAGS_SUPPORTED | (1ULL << 63));
	restamp(fs_info, block);
	if (btrfs_wib_block_valid(fs_info, block)) {
		test_err("block with an unknown flag accepted");
		goto out;
	}

	/*
	 * Layout choice.  A block with nothing stale must come out in the
	 * NARROW format with flags clear, so that a kernel predating the stale
	 * record still reads it -- refusing a log means the stripes it covers
	 * are never recovered, which is the whole thing the feature prevents.
	 * A block that does carry a stale record must come out wide.
	 */
	/* Two records of our own, so this does not depend on what ran before. */
	wib->entries[0].bytenr = 8 * BTRFS_WIB_ENTRY_SIZE;
	wib->entries[0].bitmap = 0x00f0;
	wib->entries[0].sticky = 0x0010;
	wib->entries[1].bytenr = 2 * BTRFS_WIB_ENTRY_SIZE;
	wib->entries[1].bitmap = 0;
	wib->entries[1].sticky = 0x8000;

	btrfs_wib_build_block(wib, block, 45, NULL);
	if (hdr->flags != 0) {
		test_err("a block with nothing stale was written in the wide format");
		goto out;
	}
	{
		const u32 nr = le32_to_cpu(hdr->nr_entries);
		struct btrfs_wib_entry before[8];
		int victim = -1;

		if (nr == 0 || nr > ARRAY_SIZE(before)) {
			test_err("self test needs 1..%zu entries, block has %u",
				 ARRAY_SIZE(before), nr);
			goto out;
		}
		for (u32 i = 0; i < nr; i++)
			btrfs_wib_read_entry(block, i, &before[i]);

		/* Give one live entry a stale record and rebuild. */
		for (int i = 0; i < BTRFS_WIB_MAX_ENTRIES_V1; i++) {
			if (wib->entries[i].sticky) {
				victim = i;
				break;
			}
		}
		if (victim < 0) {
			test_err("self test needs an entry with an error record");
			goto out;
		}
		wib->entries[victim].stale = wib->entries[victim].sticky;
		wib->entries[victim].stale_par = 1;

		btrfs_wib_build_block(wib, block, 46, NULL);
		if (le64_to_cpu(hdr->flags) != BTRFS_WIB_FLAG_STALE) {
			test_err("a block carrying a stale record was not marked");
			goto out;
		}
		if (le32_to_cpu(hdr->nr_entries) != nr) {
			test_err("widening the layout changed the entry count: %u, expected %u",
				 le32_to_cpu(hdr->nr_entries), nr);
			goto out;
		}
		for (u32 i = 0; i < nr; i++) {
			struct btrfs_wib_entry e;

			btrfs_wib_read_entry(block, i, &e);
			if (e.bytenr != before[i].bytenr ||
			    e.bitmap != before[i].bitmap ||
			    e.sticky != before[i].sticky) {
				test_err("entry %u did not survive widening", i);
				goto out;
			}
			if (e.bytenr == wib->entries[victim].bytenr &&
			    (e.stale != wib->entries[victim].stale ||
			     e.stale_par != wib->entries[victim].stale_par)) {
				test_err("the stale record did not round-trip");
				goto out;
			}
		}
		if (!btrfs_wib_block_valid(fs_info, block)) {
			test_err("a wide block this kernel built was rejected");
			goto out;
		}
		wib->entries[victim].stale = 0;
		wib->entries[victim].stale_par = 0;
	}
	memset(&wib->entries[0], 0, sizeof(wib->entries[0]));
	memset(&wib->entries[1], 0, sizeof(wib->entries[1]));

	btrfs_wib_build_block(wib, block, 44, NULL);
	hdr->reserved[3] = cpu_to_le64(0xdeadbeef);
	restamp(fs_info, block);
	if (btrfs_wib_block_valid(fs_info, block)) {
		test_err("block using a reserved field accepted");
		goto out;
	}

	btrfs_wib_build_block(wib, block, 45, NULL);
	hdr->block_shift = cpu_to_le32(BTRFS_WIB_BLOCK_SHIFT + 1);
	restamp(fs_info, block);
	if (btrfs_wib_block_valid(fs_info, block)) {
		test_err("block with a different granularity accepted");
		goto out;
	}

	/*
	 * An entry count that would index past the end of the slot.  The bound
	 * depends on the layout the block declares, and a block with nothing
	 * stale declares the narrow one, so use the narrow maximum -- the wide
	 * maximum is well inside it and would be accepted.
	 */
	btrfs_wib_build_block(wib, block, 46, NULL);
	hdr->nr_entries = cpu_to_le32(BTRFS_WIB_MAX_ENTRIES_V1 + 1);
	restamp(fs_info, block);
	if (btrfs_wib_block_valid(fs_info, block)) {
		test_err("block with an out of range entry count accepted");
		goto out;
	}

	/* And the same block is still good once the field is put back. */
	btrfs_wib_build_block(wib, block, 47, NULL);
	if (!btrfs_wib_block_valid(fs_info, block)) {
		test_err("rebuilt block not valid");
		goto out;
	}
	ret = 0;
out:
	kfree(block);
	return ret;
}

static int test_pending_merge(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	int ret;

	/* Simulate the union of the newest slots of several devices. */
	ret = btrfs_wib_add_pending(wib, &(struct btrfs_wib_entry){
		.bytenr = 8 * BTRFS_WIB_ENTRY_SIZE, .bitmap = 0x00f0, .sticky = 0 });
	ret |= btrfs_wib_add_pending(wib, &(struct btrfs_wib_entry){
		.bytenr = 2 * BTRFS_WIB_ENTRY_SIZE, .bitmap = 0x0001, .sticky = 0 });
	ret |= btrfs_wib_add_pending(wib, &(struct btrfs_wib_entry){
		.bytenr = 8 * BTRFS_WIB_ENTRY_SIZE, .bitmap = 0x0f00, .sticky = 0x0010 });
	ret |= btrfs_wib_add_pending(wib, &(struct btrfs_wib_entry){
		.bytenr = 2 * BTRFS_WIB_ENTRY_SIZE, .bitmap = 0x0000, .sticky = 0 });
	ret |= btrfs_wib_add_pending(wib, &(struct btrfs_wib_entry){
		.bytenr = 5 * BTRFS_WIB_ENTRY_SIZE, .bitmap = 0x0000, .sticky = 0x8000 });
	if (ret) {
		test_err("add_pending failed");
		return -EINVAL;
	}
	btrfs_wib_finalize_pending(wib);
	if (wib->nr_pending != 3) {
		test_err("pending merge produced %u entries, expected 3", wib->nr_pending);
		return -EINVAL;
	}
	if (wib->pending[0].bytenr != 2 * BTRFS_WIB_ENTRY_SIZE ||
	    wib->pending[0].bitmap != 0x0001 || wib->pending[0].sticky != 0 ||
	    wib->pending[1].bytenr != 5 * BTRFS_WIB_ENTRY_SIZE ||
	    wib->pending[1].bitmap != 0 || wib->pending[1].sticky != 0x8000 ||
	    wib->pending[2].bytenr != 8 * BTRFS_WIB_ENTRY_SIZE ||
	    wib->pending[2].bitmap != 0x0ff0 || wib->pending[2].sticky != 0x0010) {
		test_err("pending merge produced wrong entries");
		return -EINVAL;
	}
	kvfree(wib->pending);
	wib->pending = NULL;
	wib->nr_pending = 0;
	wib->max_pending = 0;
	return 0;
}

/*
 * btrfs_wib_unrecovered(): a full stripe any of whose data blocks a pending
 * record lists in flight -- a write at the crash, or one marked possibly torn
 * before it, which is written the same way -- is unrecovered for as long as
 * nothing has taken the pending set over, and only then.  A record of a plain
 * failed write is not, unless raid56_wf_all_records_torn=1.
 */
static int test_unrecovered(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool all = btrfs_wib_all_records_torn();
	const u64 base = 12 * BTRFS_WIB_ENTRY_SIZE;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	int ret;

	/*
	 * In flight over blocks 2-3, an error record over block 9, and one
	 * marked possibly torn over blocks 16-17: an error record with the
	 * blocks in flight as well.
	 */
	ret = btrfs_wib_add_pending(wib, &(struct btrfs_wib_entry){
		.bytenr = base, .bitmap = 0x000c });
	ret |= btrfs_wib_add_pending(wib, &(struct btrfs_wib_entry){
		.bytenr = base, .sticky = 0x0200 });
	ret |= btrfs_wib_add_pending(wib, &(struct btrfs_wib_entry){
		.bytenr = base, .bitmap = 0x30000, .sticky = 0x30000 });
	if (ret) {
		test_err("add_pending failed");
		ret = -EINVAL;
		goto out;
	}
	ret = -EINVAL;
	btrfs_wib_finalize_pending(wib);

	if (btrfs_wib_unrecovered(fs_info, base + 2 * blk, 2)) {
		test_err("a stripe counts as unrecovered before the load says so");
		goto out;
	}
	wib->pending_unrecovered = true;
	if (!btrfs_wib_unrecovered(fs_info, base + 2 * blk, 2) ||
	    !btrfs_wib_unrecovered(fs_info, base + 1 * blk, 2) ||
	    !btrfs_wib_unrecovered(fs_info, base + 3 * blk, 3)) {
		test_err("a stripe with a write in flight is not unrecovered");
		goto out;
	}
	if (!btrfs_wib_unrecovered(fs_info, base + 16 * blk, 2)) {
		test_err("a stripe with a record marked possibly torn is not unrecovered");
		goto out;
	}
	if (btrfs_wib_unrecovered(fs_info, base + 8 * blk, 2) != all) {
		if (all)
			test_err("raid56_wf_all_records_torn is set, yet a stripe with an error record is not unrecovered");
		else
			test_err("a stripe with only the record of a failed write is unrecovered");
		goto out;
	}
	if (btrfs_wib_unrecovered(fs_info, base + 4 * blk, 4) ||
	    btrfs_wib_unrecovered(fs_info, base + 10 * blk, 3) ||
	    btrfs_wib_unrecovered(fs_info, base + BTRFS_WIB_ENTRY_SIZE, 3) ||
	    btrfs_wib_unrecovered(fs_info, base - 2 * blk, 2)) {
		test_err("a stripe no record lists is unrecovered");
		goto out;
	}
	/* What the first recovery does to it (wib_stop_consulting_pending()). */
	wib->pending_unrecovered = false;
	if (btrfs_wib_unrecovered(fs_info, base + 2 * blk, 2)) {
		test_err("a stripe is still unrecovered once recovery took over");
		goto out;
	}
	ret = 0;
out:
	wib->pending_unrecovered = false;
	kvfree(wib->pending);
	wib->pending = NULL;
	wib->nr_pending = 0;
	wib->max_pending = 0;
	return ret;
}

/*
 * The recovery takes the records read at mount over a full stripe at a time
 * (btrfs_wib_take_pending()): a stripe it has taken is the live table's, and
 * no longer refused as unrecovered or read stale from @pending, while every
 * stripe it has not reached still is -- the mount it runs on is read-only and
 * serving reads, and stays so if the recovery stops early.  A stripe given
 * back answers from @pending again, its stale marks counted once.
 */
static int test_recovery_takes_stripes(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 base = 20 * BTRFS_WIB_ENTRY_SIZE;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	const int nr_stale = atomic_read(&wib->nr_stale);
	struct btrfs_wib_stripe_state st;
	unsigned long flags;
	int ret;

	/*
	 * Three full stripes of two data blocks each.  A: a write in flight.
	 * B: an error record, column 1 and parity 0 stale.  C: a write in
	 * flight, column 0 stale.
	 */
	ret = btrfs_wib_add_pending(wib, &(struct btrfs_wib_entry){
		.bytenr = base, .bitmap = 0x33, .sticky = 0x0c, .stale = 0x18,
		.stale_par = 0x04 });
	if (ret) {
		test_err("add_pending failed");
		return ret;
	}
	btrfs_wib_finalize_pending(wib);
	ret = -ENOMEM;
	wib->pending_taken = kvcalloc(wib->nr_pending, sizeof(u64), GFP_KERNEL);
	if (!wib->pending_taken)
		goto out;
	ret = -EINVAL;
	/* As btrfs_wib_load() leaves them. */
	spin_lock_irqsave(&wib->lock, flags);
	wib->consult_pending = true;
	wib->nr_pending_stale = 3;
	atomic_add(3, &wib->nr_stale);
	wib->pending_unrecovered = true;
	spin_unlock_irqrestore(&wib->lock, flags);

	if (!btrfs_wib_unrecovered(fs_info, base, 2) ||
	    !btrfs_wib_stale(fs_info, base + 3 * blk) ||
	    !btrfs_wib_stripe_state(fs_info, base + 2 * blk, 2, 1, &st) ||
	    st.bad_parity != 1) {
		test_err("the records read at mount do not answer before the recovery");
		goto out;
	}

	btrfs_wib_take_pending(wib, base, 2 * blk, true);
	btrfs_wib_take_pending(wib, base + 2 * blk, 2 * blk, true);
	btrfs_wib_take_pending(wib, base + 2 * blk, 2 * blk, true);
	if (btrfs_wib_unrecovered(fs_info, base, 2) ||
	    btrfs_wib_stale(fs_info, base + 3 * blk) ||
	    btrfs_wib_stripe_state(fs_info, base + 2 * blk, 2, 1, &st)) {
		test_err("a stripe the recovery took over still answers from the records read at mount");
		goto out;
	}
	if (!btrfs_wib_unrecovered(fs_info, base + 4 * blk, 2) ||
	    !btrfs_wib_stale(fs_info, base + 4 * blk)) {
		test_err("a stripe the recovery has not reached no longer answers from the records read at mount");
		goto out;
	}
	if (wib->nr_pending_stale != 1 || atomic_read(&wib->nr_stale) != nr_stale + 1) {
		test_err("taking two stripes over left %u pending stale marks, %d in all, expected 1, %d",
			 wib->nr_pending_stale, atomic_read(&wib->nr_stale), nr_stale + 1);
		goto out;
	}

	/* Its recovery did not happen after all. */
	btrfs_wib_take_pending(wib, base + 2 * blk, 2 * blk, false);
	btrfs_wib_take_pending(wib, base + 2 * blk, 2 * blk, false);
	if (!btrfs_wib_stale(fs_info, base + 3 * blk) ||
	    !btrfs_wib_stripe_state(fs_info, base + 2 * blk, 2, 1, &st) ||
	    st.bad_parity != 1 || btrfs_wib_unrecovered(fs_info, base, 2)) {
		test_err("a stripe given back does not answer from the records read at mount again");
		goto out;
	}
	if (wib->nr_pending_stale != 3 || atomic_read(&wib->nr_stale) != nr_stale + 3) {
		test_err("giving a stripe back left %u pending stale marks, %d in all, expected 3, %d",
			 wib->nr_pending_stale, atomic_read(&wib->nr_stale), nr_stale + 3);
		goto out;
	}
	ret = 0;
out:
	spin_lock_irqsave(&wib->lock, flags);
	if (wib->consult_pending)
		atomic_sub(wib->nr_pending_stale, &wib->nr_stale);
	wib->consult_pending = false;
	wib->nr_pending_stale = 0;
	wib->pending_unrecovered = false;
	spin_unlock_irqrestore(&wib->lock, flags);
	kvfree(wib->pending_taken);
	wib->pending_taken = NULL;
	kvfree(wib->pending);
	wib->pending = NULL;
	wib->nr_pending = 0;
	wib->max_pending = 0;
	return ret;
}

/*
 * raid56_health while the log read at mount lists writes no recovery has taken
 * over: the reads they may have torn are refused (btrfs_wib_unrecovered()), so
 * the state is not "ok" and the thing to do is a read-write mount -- unless
 * raid56_wf_unrecovered_as_ambiguous=1, under which nothing changes.
 */
static int test_unrecovered_health(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool old = btrfs_wib_unrecovered_as_ambiguous();
	char *buf = (char *)get_zeroed_page(GFP_KERNEL);
	char *before = (char *)get_zeroed_page(GFP_KERNEL);
	int ret = -EINVAL;

	if (!buf || !before) {
		ret = -ENOMEM;
		goto out;
	}
	btrfs_raid56_health_show(fs_info, before);
	if (strstr(before, "\naction mount-rw\n")) {
		test_err("raid56_health asks for a read-write mount with nothing unrecovered");
		goto out;
	}
	wib->pending_unrecovered = true;
	btrfs_raid56_health_show(fs_info, buf);
	if (old) {
		if (strcmp(buf, before)) {
			test_err("raid56_wf_unrecovered_as_ambiguous is set, yet raid56_health changed with unrecovered records");
			goto out;
		}
	} else if (strstr(buf, "state ok\n") || !strstr(buf, "\naction mount-rw\n")) {
		test_err("raid56_health does not ask for a read-write mount while unrecovered records are refused");
		goto out;
	}
	ret = 0;
out:
	wib->pending_unrecovered = false;
	free_page((unsigned long)before);
	free_page((unsigned long)buf);
	return ret;
}

/*
 * A log from a kernel that does not mark the writes a failed flush may have
 * torn -- one from before the mark, whose readd turned them into plain error
 * records -- cannot say which of its error records is one.  Loaded, every one
 * of them is possibly torn (btrfs_wib_unrecovered()); loaded from a block
 * that says its writer marks (BTRFS_WIB_TORN_MARKING), none is.  Every block
 * this kernel builds says so, unless raid56_wf_log_unmarked=1, and
 * raid56_wf_trust_unmarked_log=1 reads a block that does not as one that does.
 */
static int test_unmarked_log(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool unmarked = btrfs_wib_log_unmarked();
	const bool trust = btrfs_wib_trust_unmarked_log();
	const bool all = btrfs_wib_all_records_torn();
	const u64 base = 975ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	struct btrfs_wib_entry *e = NULL;
	unsigned int nr_sticky = 0;
	unsigned int nr_torn;
	void *block;
	int ret;

	block = kzalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!block)
		return -ENOMEM;

	/* A failed write's record over block 4, a write in flight over block 8. */
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES && !e; i++)
		if (!(wib->entries[i].bitmap | wib->entries[i].sticky))
			e = &wib->entries[i];
	if (!e) {
		test_err("self test needs a free entry");
		ret = -EINVAL;
		goto out;
	}
	memset(e, 0, sizeof(*e));
	e->bytenr = base;
	e->bitmap = 0x100;
	e->sticky = 0x10;
	ret = btrfs_wib_build_block(wib, block, 60, NULL);
	memset(e, 0, sizeof(*e));
	if (ret) {
		test_err("building the block failed: %d", ret);
		goto out;
	}
	ret = -EINVAL;
	if (!btrfs_wib_block_valid(fs_info, block)) {
		test_err("a block this kernel built was rejected");
		goto out;
	}
	if (btrfs_wib_block_marks_torn(block) == unmarked) {
		if (unmarked)
			test_err("raid56_wf_log_unmarked is set, yet the block says its writer marks");
		else
			test_err("a block this kernel built does not say its writer marks");
		goto out;
	}
	/* Whatever else the live table holds went into the block too. */
	for (u32 i = 0; i < block_nr_entries(block); i++) {
		struct btrfs_wib_entry be;

		btrfs_wib_read_entry(block, i, &be);
		nr_sticky += hweight64(be.sticky);
	}

	/* As built, then as a kernel from before the mark would have written it. */
	for (int pass = 0; pass < 2; pass++) {
		const bool marked = pass == 0 && !unmarked;
		const bool torn = !marked && !trust;

		if (pass == 1) {
			memset(block + BTRFS_WIB_TRAILER_OFFSET, 0, sizeof(__le64));
			restamp(fs_info, block);
			if (!btrfs_wib_block_valid(fs_info, block) ||
			    btrfs_wib_block_marks_torn(block)) {
				test_err("a block without the marker is not read as a valid unmarked one");
				goto out;
			}
		}
		if (btrfs_wib_load_block(wib, block, true, &nr_torn)) {
			test_err("loading the block failed");
			goto out;
		}
		btrfs_wib_finalize_pending(wib);
		wib->pending_unrecovered = true;
		if (nr_torn != (torn ? nr_sticky : 0)) {
			test_err("%s block: %u error records loaded possibly torn, expected %u",
				 marked ? "a marked" : "an unmarked", nr_torn,
				 torn ? nr_sticky : 0);
			goto out;
		}
		if (!btrfs_wib_unrecovered(fs_info, base + 8 * blk, 2)) {
			test_err("a stripe with a write in flight is not unrecovered");
			goto out;
		}
		if (btrfs_wib_unrecovered(fs_info, base + 4 * blk, 2) != (torn || all)) {
			if (marked)
				test_err("a failed write's record from a writer that marks is taken for possibly torn");
			else if (trust)
				test_err("raid56_wf_trust_unmarked_log is set, yet an unmarked block's error record is possibly torn");
			else
				test_err("an unmarked block's error record is not possibly torn");
			goto out;
		}
		wib->pending_unrecovered = false;
		kvfree(wib->pending);
		wib->pending = NULL;
		wib->nr_pending = 0;
		wib->max_pending = 0;
	}
	if (trust)
		test_msg("raid56_wf_trust_unmarked_log is set: trusted, as expected");
	ret = 0;
out:
	wib->pending_unrecovered = false;
	kvfree(wib->pending);
	wib->pending = NULL;
	wib->nr_pending = 0;
	wib->max_pending = 0;
	kfree(block);
	return ret;
}

static int test_log_full(struct btrfs_fs_info *fs_info)
{
	unsigned long flags;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 straddling = 50 * BTRFS_WIB_ENTRY_SIZE + 63 * BTRFS_WIB_BLOCK_SIZE;
	int ret;

	/* Fill every entry with a distinct region. */
	for (u64 i = 0; i < BTRFS_WIB_MAX_ENTRIES_V1; i++) {
		ret = btrfs_wib_mark(fs_info, (i + 100) * BTRFS_WIB_ENTRY_SIZE,
				     BTRFS_WIB_BLOCK_SIZE);
		if (ret) {
			test_err("mark %llu failed: %d", i, ret);
			return ret;
		}
	}

	/* A region already present still fits. */
	spin_lock_irqsave(&wib->lock, flags);
	ret = btrfs_wib_try_mark(wib, 100 * BTRFS_WIB_ENTRY_SIZE + SZ_1M, BTRFS_WIB_BLOCK_SIZE);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret) {
		test_err("mark of a present region failed on a full log: %d", ret);
		return -EINVAL;
	}

	/* A new region must be refused, not silently dropped. */
	spin_lock_irqsave(&wib->lock, flags);
	ret = btrfs_wib_try_mark(wib, 0, BTRFS_WIB_BLOCK_SIZE);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret != -ENOSPC || btrfs_wib_can_mark(wib, 0, BTRFS_WIB_BLOCK_SIZE)) {
		test_err("full log accepted a new region: %d", ret);
		return -EINVAL;
	}

	/* Finishing a stripe frees its entry for reuse. */
	btrfs_wib_done(fs_info, 150 * BTRFS_WIB_ENTRY_SIZE, BTRFS_WIB_BLOCK_SIZE, false);
	if (!btrfs_wib_can_mark(wib, 0, BTRFS_WIB_BLOCK_SIZE)) {
		test_err("freed entry not seen as available");
		return -EINVAL;
	}

	/*
	 * A stripe straddling two entries needs two free ones: with one it
	 * must be refused as a whole (and nothing left behind), so that a
	 * waiter is not woken to fail again.
	 */
	if (btrfs_wib_can_mark(wib, straddling, 2 * BTRFS_WIB_BLOCK_SIZE)) {
		test_err("straddling stripe accepted with a single free entry");
		return -EINVAL;
	}
	spin_lock_irqsave(&wib->lock, flags);
	ret = btrfs_wib_try_mark(wib, straddling, 2 * BTRFS_WIB_BLOCK_SIZE);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret != -ENOSPC) {
		test_err("straddling stripe marked with a single free entry: %d", ret);
		return -EINVAL;
	}
	ret = btrfs_wib_mark(fs_info, 0, BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("mark after freeing an entry failed (refused straddling stripe left bits behind?): %d",
			 ret);
		return ret;
	}
	btrfs_wib_done(fs_info, 0, BTRFS_WIB_BLOCK_SIZE, false);
	btrfs_wib_done(fs_info, 151 * BTRFS_WIB_ENTRY_SIZE, BTRFS_WIB_BLOCK_SIZE, false);
	if (!btrfs_wib_can_mark(wib, straddling, 2 * BTRFS_WIB_BLOCK_SIZE)) {
		test_err("straddling stripe refused with two free entries");
		return -EINVAL;
	}
	ret = btrfs_wib_mark(fs_info, straddling, 2 * BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("straddling stripe mark failed: %d", ret);
		return ret;
	}

	/*
	 * The on-disk block lists every region ever marked since the last
	 * drop; when the union does not fit anymore a mark-time commit has
	 * to flush and drop the finished ones itself.
	 */
	btrfs_wib_done(fs_info, straddling, 2 * BTRFS_WIB_BLOCK_SIZE, false);
	btrfs_wib_done(fs_info, 100 * BTRFS_WIB_ENTRY_SIZE + SZ_1M, BTRFS_WIB_BLOCK_SIZE, false);
	for (u64 i = 0; i < BTRFS_WIB_MAX_ENTRIES_V1; i++)
		btrfs_wib_done(fs_info, (i + 100) * BTRFS_WIB_ENTRY_SIZE,
			       BTRFS_WIB_BLOCK_SIZE, false);
	ret = btrfs_wib_mark(fs_info, 0, BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("mark with a full on-disk block failed: %d", ret);
		return ret;
	}
	if (block_nr_entries(wib->last) != 1 ||
	    atomic64_read(&wib->stat_commit_flushes) == 0) {
		test_err("mark-time commit did not drop finished stripes when the block was full (%u entries)",
			 block_nr_entries(wib->last));
		return -EINVAL;
	}
	btrfs_wib_done(fs_info, 0, BTRFS_WIB_BLOCK_SIZE, false);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after cleanup");
		return -EINVAL;
	}
	return 0;
}

static int test_sticky(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 stripe = 20 * BTRFS_WIB_ENTRY_SIZE;
	int ret;

	/* A stripe whose write failed stays in the on-disk log as an error record. */
	ret = btrfs_wib_mark(fs_info, stripe, 3 * BTRFS_WIB_BLOCK_SIZE);
	if (ret)
		return ret;
	btrfs_wib_done(fs_info, stripe, 3 * BTRFS_WIB_BLOCK_SIZE, true);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 1) {
		test_err("failed stripe dropped from the log");
		return -EINVAL;
	}
	ret = check_block_entry(wib->last, 0, stripe, 0, 0x7);
	if (ret)
		return ret;

	/* A new write to the same stripe is listed in both kinds. */
	ret = btrfs_wib_mark(fs_info, stripe, 3 * BTRFS_WIB_BLOCK_SIZE);
	if (ret)
		return ret;
	ret = check_block_entry(wib->last, 0, stripe, 0x7, 0x7);
	if (ret)
		return ret;
	btrfs_wib_done(fs_info, stripe, 3 * BTRFS_WIB_BLOCK_SIZE, false);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	ret = check_block_entry(wib->last, 0, stripe, 0, 0x7);
	if (ret)
		return ret;

	/* Recovery clears the error record once the stripe is verified. */
	btrfs_wib_clear_sticky(fs_info, stripe, 3 * BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("cleared error record still in the log");
		return -EINVAL;
	}

	/* Sticky entries are evicted when the log is full. */
	btrfs_wib_add_sticky(fs_info, stripe, 3 * BTRFS_WIB_BLOCK_SIZE);
	for (u64 i = 0; i < BTRFS_WIB_MAX_ENTRIES_V1; i++) {
		ret = btrfs_wib_mark(fs_info, (i + 300) * BTRFS_WIB_ENTRY_SIZE,
				     BTRFS_WIB_BLOCK_SIZE);
		if (ret) {
			test_err("mark %llu with a sticky entry present failed: %d", i, ret);
			return ret;
		}
	}
	if (atomic64_read(&wib->stat_sticky_evicted) != 1) {
		test_err("sticky entry not evicted");
		return -EINVAL;
	}
	for (u64 i = 0; i < BTRFS_WIB_MAX_ENTRIES_V1; i++)
		btrfs_wib_done(fs_info, (i + 300) * BTRFS_WIB_ENTRY_SIZE,
			       BTRFS_WIB_BLOCK_SIZE, false);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after sticky test");
		return -EINVAL;
	}
	return 0;
}

/*
 * Disabling must keep the log maintained until a superblock without the
 * feature flag is durable, i.e. until the commit after the one that writes
 * such a superblock.
 */
static int test_disable_ordering(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 stripe = 30 * BTRFS_WIB_ENTRY_SIZE;
	int ret;

	btrfs_set_super_compat_ro_flags(fs_info->super_for_commit,
					BTRFS_FEATURE_COMPAT_RO_RAID56_WRITE_INTENT);
	btrfs_wib_disable(fs_info);
	if (!wib->enabled) {
		test_err("disable took effect immediately");
		return -EINVAL;
	}

	/* This commit still writes a superblock with the flag: keep going. */
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	ret = btrfs_wib_mark(fs_info, stripe, BTRFS_WIB_BLOCK_SIZE);
	if (ret)
		return ret;
	if (!wib->enabled || block_nr_entries(wib->last) != 1) {
		test_err("log not maintained while the flag is still on disk");
		return -EINVAL;
	}
	btrfs_wib_done(fs_info, stripe, BTRFS_WIB_BLOCK_SIZE, false);

	/* The flag is cleared for the next superblock: armed, still logging. */
	btrfs_set_super_compat_ro_flags(fs_info->super_for_commit, 0);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (!wib->enabled) {
		test_err("disabled before the superblock without the flag was written");
		return -EINVAL;
	}
	/* The following commit knows it is durable. */
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (wib->enabled) {
		test_err("not disabled after the superblock without the flag was written");
		return -EINVAL;
	}

	/* Enable again for the remaining tests (and the enable request path). */
	btrfs_wib_request_enable(fs_info, true);
	if (!wib->enable_requested ||
	    !btrfs_fs_compat_ro(fs_info, RAID56_WRITE_INTENT)) {
		test_err("enable request did not set the feature flag");
		return -EINVAL;
	}
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (!wib->enabled || wib->enable_requested) {
		test_err("enable request not honoured at commit");
		return -EINVAL;
	}
	return 0;
}

/* The entry-aligned base of @logical; mirrors wib_entry_bytenr(). */
static u64 wib_test_entry_base(u64 logical)
{
	return round_down(logical, BTRFS_WIB_ENTRY_SIZE);
}

/* The in-flight bits a block lists for region @bytenr, possibly torn included. */
static u64 block_bitmap(const void *block, u64 bytenr)
{
	for (u32 i = 0; i < block_nr_entries(block); i++) {
		struct btrfs_wib_entry e;

		btrfs_wib_read_entry(block, i, &e);
		if (e.bytenr == bytenr)
			return e.bitmap;
	}
	return 0;
}

/* The error records a block lists for region @bytenr. */
static u64 block_error(const void *block, u64 bytenr)
{
	for (u32 i = 0; i < block_nr_entries(block); i++) {
		struct btrfs_wib_entry e;

		btrfs_wib_read_entry(block, i, &e);
		if (e.bytenr == bytenr)
			return e.sticky;
	}
	return 0;
}

/* The live entry for @bytenr, or NULL.  Tests only; no locking needed here. */
static struct btrfs_wib_entry *find_live_entry(struct btrfs_wib *wib, u64 bytenr)
{
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		struct btrfs_wib_entry *e = &wib->entries[i];

		if ((e->bitmap | e->sticky) && e->bytenr == bytenr)
			return e;
	}
	return NULL;
}

/* Drop @nr marks starting at entry @base and empty the log. */
static int drain_marks(struct btrfs_fs_info *fs_info, u64 base, u64 nr)
{
	for (u64 i = 0; i < nr; i++)
		btrfs_wib_done(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
			       BTRFS_WIB_BLOCK_SIZE, false);
	btrfs_wib_commit_prepare(fs_info);
	return btrfs_wib_commit(fs_info, true);
}

/*
 * When the log is full, the record that NAMES the member a write went wrong on
 * must be the last one spent, not the first one found.
 *
 * A record with only @sticky says something went wrong somewhere in the
 * stripe, which a scrub rediscovers by reading it.  A record with @stale says
 * WHICH member is wrong, and for data with no checksum nothing else in the
 * system can say that -- a scrub that loses it cannot tell a stale data column
 * from a stale parity, and the two need opposite repairs.  Losing the vague
 * one costs a rescan; losing the specific one costs the evidence.
 *
 * And a named record is never spent at all: with nothing vague left, a new
 * mark waits for a write in flight to finish, or fails -- see
 * test_named_records_stay().  wib_count_entries_locked() counts only what
 * wib_evictable() allows, so the room btrfs_wib_try_mark() asserts is there.
 */
static int test_evict_precedence(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 vague = 700ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 named = 701ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 base = 800;
	u64 evicted0, stale_evicted0;
	int ret;

	evicted0 = atomic64_read(&wib->stat_sticky_evicted);
	stale_evicted0 = atomic64_read(&wib->stat_stale_evicted);

	btrfs_wib_add_sticky(fs_info, vague, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_add_sticky(fs_info, named, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_mark_stale(fs_info, named, BTRFS_WIB_BLOCK_SIZE);

	if (!find_live_entry(wib, vague) || !find_live_entry(wib, named)) {
		test_err("sticky records were not created");
		return -EINVAL;
	}
	if (!btrfs_wib_any_stale(fs_info)) {
		test_err("the named record did not register as stale");
		return -EINVAL;
	}

	/*
	 * A named record is live, so the log must be writable in the wide
	 * layout and the live bound is BTRFS_WIB_MAX_ENTRIES, not the size of
	 * the table.  Two slots are taken, so the (MAX_ENTRIES - 1)th fresh
	 * region is the one that has to displace something.
	 */
	for (u64 i = 0; i < BTRFS_WIB_MAX_ENTRIES - 1; i++) {
		ret = btrfs_wib_mark(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				     BTRFS_WIB_BLOCK_SIZE);
		if (ret) {
			test_err("mark %llu failed: %d", i, ret);
			return ret;
		}
	}

	if (atomic64_read(&wib->stat_sticky_evicted) != evicted0 + 1) {
		test_err("expected exactly one eviction, saw %llu",
			 atomic64_read(&wib->stat_sticky_evicted) - evicted0);
		return -EINVAL;
	}
	if (find_live_entry(wib, vague)) {
		test_err("the record naming no member survived; the wrong one was spent");
		return -EINVAL;
	}
	if (!find_live_entry(wib, named)) {
		test_err("the record naming a member was evicted first");
		return -EINVAL;
	}
	if (atomic64_read(&wib->stat_stale_evicted) != stale_evicted0) {
		test_err("stale_evicted moved while only a vague record was dropped");
		return -EINVAL;
	}
	if (!btrfs_wib_any_stale(fs_info)) {
		test_err("the surviving named record stopped counting as stale");
		return -EINVAL;
	}

	/*
	 * Nothing vague left, and the named record is not spendable: room
	 * comes only from the writes in flight.  Finish them, and the next
	 * mark fits beside the named record.
	 */
	for (u64 i = 0; i < BTRFS_WIB_MAX_ENTRIES - 1; i++)
		btrfs_wib_done(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
			       BTRFS_WIB_BLOCK_SIZE, false);
	ret = btrfs_wib_mark(fs_info, (base + BTRFS_WIB_MAX_ENTRIES) * BTRFS_WIB_ENTRY_SIZE,
			     BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("mark after the writes in flight finished failed: %d", ret);
		return ret;
	}
	if (!find_live_entry(wib, named)) {
		test_err("the named record was spent to make room");
		return -EINVAL;
	}
	if (atomic64_read(&wib->stat_stale_evicted) != stale_evicted0) {
		test_err("stale_evicted moved: a named record was dropped");
		return -EINVAL;
	}
	if (!btrfs_wib_any_stale(fs_info)) {
		test_err("the named record stopped counting as stale");
		return -EINVAL;
	}

	/* A repair retires it. */
	btrfs_wib_clear_sticky(fs_info, named, BTRFS_WIB_BLOCK_SIZE);
	if (btrfs_wib_any_stale(fs_info)) {
		test_err("nr_stale was not decremented when the named record was retired");
		return -EINVAL;
	}
	ret = drain_marks(fs_info, base + BTRFS_WIB_MAX_ENTRIES, 1);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the eviction precedence test");
		return -EINVAL;
	}
	return 0;
}

/*
 * A log full of records that name stale members, with no write in flight:
 * nothing can make room, so a new mark fails at once -- it does not wait a
 * minute first -- and no record is dropped.  The records stay until repairs
 * retire them.
 */
static int test_named_records_stay(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 base = 1200;
	const u64 stale_evicted0 = atomic64_read(&wib->stat_stale_evicted);
	const u64 full0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_LOG_FULL]);
	unsigned long start;
	int ret;

	/* A negative-control run asked for the old eviction on purpose. */
	if (btrfs_wib_evicts_naming()) {
		test_msg("raid56_evict_naming is set, skipping the named-records test");
		return 0;
	}

	for (u64 i = 0; i < BTRFS_WIB_MAX_ENTRIES; i++) {
		const u64 logical = (base + i) * BTRFS_WIB_ENTRY_SIZE;

		btrfs_wib_add_sticky(fs_info, logical, BTRFS_WIB_BLOCK_SIZE);
		btrfs_wib_mark_stale(fs_info, logical, BTRFS_WIB_BLOCK_SIZE);
		if (!find_live_entry(wib, logical)) {
			test_err("named record %llu was not created", i);
			return -EINVAL;
		}
	}

	start = jiffies;
	ret = btrfs_wib_mark(fs_info, (base + BTRFS_WIB_MAX_ENTRIES) * BTRFS_WIB_ENTRY_SIZE,
			     BTRFS_WIB_BLOCK_SIZE);
	if (ret != -EIO) {
		test_err("mark into a log full of named records returned %d, expected -EIO", ret);
		return -EINVAL;
	}
	if (time_after(jiffies, start + 5 * HZ)) {
		test_err("mark into a log full of named records waited %u ms with nothing in flight",
			 jiffies_to_msecs(jiffies - start));
		return -EINVAL;
	}
	if (atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_LOG_FULL]) != full0 + 1) {
		test_err("the failed mark raised no log-full alert");
		return -EINVAL;
	}
	if (atomic64_read(&wib->stat_stale_evicted) != stale_evicted0) {
		test_err("a named record was dropped to make room");
		return -EINVAL;
	}
	for (u64 i = 0; i < BTRFS_WIB_MAX_ENTRIES; i++) {
		if (!find_live_entry(wib, (base + i) * BTRFS_WIB_ENTRY_SIZE)) {
			test_err("named record %llu is gone", i);
			return -EINVAL;
		}
	}

	for (u64 i = 0; i < BTRFS_WIB_MAX_ENTRIES; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       BTRFS_WIB_BLOCK_SIZE);
	if (btrfs_wib_any_stale(fs_info)) {
		test_err("stale marks left after retiring every named record");
		return -EINVAL;
	}
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the named-records test");
		return -EINVAL;
	}
	return 0;
}

/* Forget the repairs asked for so far: none runs here (see btrfs_test_raid56_wib()). */
static void clear_repairs(struct btrfs_wib *wib)
{
	spin_lock(&wib->repair_lock);
	wib->repair_nr = 0;
	spin_unlock(&wib->repair_lock);
}

/*
 * A log full of records that only say a write may have been torn -- what a
 * failed flush that could not name the device leaves -- with every device
 * there.  No repair retires one and no write finishing does, so the log
 * spends them, after the vague records and with the alert, rather than fail
 * every write into a new region until a scrub.  raid56_wf_torn_unevictable=1
 * keeps them: the write fails at once, as it did.
 */
static int test_torn_spent_last(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool keep = btrfs_wib_torn_unevictable() && !btrfs_wib_evicts_naming();
	const u64 evicted0 = atomic64_read(&wib->stat_sticky_evicted);
	const u64 dropped0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]);
	const u64 full0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_LOG_FULL]);
	const u64 vague = 4199ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 fresh = 4400ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 base = 4200;
	const u32 nr = BTRFS_WIB_MAX_ENTRIES_V1;
	unsigned long flags;
	u32 left = 0;
	int ret;

	clear_repairs(wib);
	btrfs_wib_add_sticky(fs_info, vague, BTRFS_WIB_BLOCK_SIZE);
	for (u32 i = 0; i < nr - 1; i++)
		btrfs_wib_add_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				     BTRFS_WIB_BLOCK_SIZE);
	spin_lock_irqsave(&wib->lock, flags);
	for (u32 i = 0; i < nr - 1; i++) {
		struct btrfs_wib_entry *e = find_live_entry(wib, (base + i) * BTRFS_WIB_ENTRY_SIZE);

		if (e)
			e->torn = e->sticky;
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	if (!find_live_entry(wib, vague) ||
	    !find_live_entry(wib, (base + nr - 2) * BTRFS_WIB_ENTRY_SIZE)) {
		test_err("the records were not created");
		return -EINVAL;
	}

	/* The vague record goes first. */
	ret = btrfs_wib_mark(fs_info, fresh, BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("mark with a vague record to spend failed: %d", ret);
		return ret;
	}
	btrfs_wib_done(fs_info, fresh, BTRFS_WIB_BLOCK_SIZE, false);
	if (find_live_entry(wib, vague) ||
	    atomic64_read(&wib->stat_sticky_evicted) != evicted0 + 1) {
		test_err("the vague record was not the one spent");
		return -EINVAL;
	}
	btrfs_wib_add_sticky(fs_info, (base + nr - 1) * BTRFS_WIB_ENTRY_SIZE,
			     BTRFS_WIB_BLOCK_SIZE);
	spin_lock_irqsave(&wib->lock, flags);
	find_live_entry(wib, (base + nr - 1) * BTRFS_WIB_ENTRY_SIZE)->torn = 0x1;
	spin_unlock_irqrestore(&wib->lock, flags);

	/* Nothing but possibly torn records left, nothing in flight. */
	ret = btrfs_wib_mark(fs_info, fresh + BTRFS_WIB_ENTRY_SIZE, BTRFS_WIB_BLOCK_SIZE);
	for (u32 i = 0; i < nr; i++)
		if (find_live_entry(wib, (base + i) * BTRFS_WIB_ENTRY_SIZE))
			left++;
	if (keep) {
		if (ret != -EIO || left != nr ||
		    atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_LOG_FULL]) != full0 + 1) {
			test_err("raid56_wf_torn_unevictable is set, yet the mark returned %d with %u of %u records left",
				 ret, left, nr);
			return -EINVAL;
		}
		test_msg("raid56_wf_torn_unevictable is set: possibly torn records kept, the write failed, as expected");
	} else {
		if (ret) {
			test_err("mark into a log full of possibly torn records failed: %d", ret);
			return ret;
		}
		btrfs_wib_done(fs_info, fresh + BTRFS_WIB_ENTRY_SIZE, BTRFS_WIB_BLOCK_SIZE, false);
		if (left != nr - 1 ||
		    atomic64_read(&wib->stat_sticky_evicted) != evicted0 + 2 ||
		    atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]) != dropped0 + 2) {
			test_err("expected one possibly torn record spent, with the alert: %u of %u left",
				 left, nr);
			return -EINVAL;
		}
	}

	for (u32 i = 0; i < nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the possibly-torn test");
		return -EINVAL;
	}
	return 0;
}

struct wib_done_later {
	struct delayed_work work;
	struct btrfs_fs_info *fs_info;
	u64 logical;
};

static void wib_done_later_fn(struct work_struct *work)
{
	struct wib_done_later *d = container_of(to_delayed_work(work),
						struct wib_done_later, work);

	btrfs_wib_done(d->fs_info, d->logical, BTRFS_WIB_BLOCK_SIZE, false);
}

/*
 * The same log full of possibly torn records, but with a write in flight: it
 * frees its slot when it finishes, so a write into a new region waits for that
 * and spends nothing, rather than drop a mark a read refusal and the next mount
 * depend on.  raid56_wf_torn_spent_eagerly=1 spends one at once.  A write in
 * flight into a region the log holds a record of frees nothing, though: with
 * only that one, a write into a new region spends a record at once rather
 * than wait a minute for nothing.
 */
static int test_torn_waits_for_write(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool eager = btrfs_wib_evicts_naming() ||
			   (btrfs_wib_torn_spent_eagerly() && !btrfs_wib_torn_unevictable());
	const u64 evicted0 = atomic64_read(&wib->stat_sticky_evicted);
	const u64 dropped0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]);
	const u64 flying = 4600ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 fresh = 4601ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 base = 4610;
	const u32 nr = BTRFS_WIB_MAX_ENTRIES_V1;
	const u64 extra = 4602ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 fresh2 = 4603ULL * BTRFS_WIB_ENTRY_SIZE;
	struct wib_done_later later = { .fs_info = fs_info, .logical = flying };
	struct btrfs_wib_entry *e;
	unsigned long flags;
	unsigned long start;
	unsigned int waited;
	u32 left = 0;
	int ret;

	clear_repairs(wib);
	for (u32 i = 0; i < nr - 1; i++)
		btrfs_wib_add_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				     BTRFS_WIB_BLOCK_SIZE);
	spin_lock_irqsave(&wib->lock, flags);
	for (u32 i = 0; i < nr - 1; i++) {
		struct btrfs_wib_entry *e = find_live_entry(wib, (base + i) * BTRFS_WIB_ENTRY_SIZE);

		if (e)
			e->torn = e->sticky;
	}
	spin_unlock_irqrestore(&wib->lock, flags);
	ret = btrfs_wib_mark(fs_info, flying, BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("mark into the last free slot failed: %d", ret);
		return ret;
	}
	if (atomic64_read(&wib->stat_sticky_evicted) != evicted0) {
		test_err("a record was spent while a slot was free");
		return -EINVAL;
	}

	/* Full: possibly torn records, and a write in flight. */
	INIT_DELAYED_WORK_ONSTACK(&later.work, wib_done_later_fn);
	schedule_delayed_work(&later.work, msecs_to_jiffies(300));
	start = jiffies;
	ret = btrfs_wib_mark(fs_info, fresh, BTRFS_WIB_BLOCK_SIZE);
	waited = jiffies_to_msecs(jiffies - start);
	flush_delayed_work(&later.work);
	destroy_delayed_work_on_stack(&later.work);
	if (ret) {
		test_err("mark into a full log with a write in flight failed: %d", ret);
		return ret;
	}
	btrfs_wib_done(fs_info, fresh, BTRFS_WIB_BLOCK_SIZE, false);
	for (u32 i = 0; i < nr - 1; i++)
		if (find_live_entry(wib, (base + i) * BTRFS_WIB_ENTRY_SIZE))
			left++;
	if (eager) {
		if (left != nr - 2 || atomic64_read(&wib->stat_sticky_evicted) != evicted0 + 1 ||
		    atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]) != dropped0 + 1) {
			test_err("possibly torn records spent eagerly, yet %u of %u are left",
				 left, nr - 1);
			return -EINVAL;
		}
		test_msg("possibly torn records spent eagerly: one spent at once, as expected");
	} else if (left != nr - 1 || atomic64_read(&wib->stat_sticky_evicted) != evicted0 ||
		   atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]) != dropped0) {
		test_err("a possibly torn record was spent while a write in flight could make room (%u of %u left, waited %u ms)",
			 left, nr - 1, waited);
		return -EINVAL;
	} else if (waited < 200) {
		test_err("the mark did not wait for the write in flight (%u ms)", waited);
		return -EINVAL;
	}

	/*
	 * Full again, the last slot a possibly torn record too, and the only
	 * write in flight one into a recorded region.
	 */
	if (eager || btrfs_wib_torn_unevictable()) {
		test_msg("a knob is set: skipping the write in flight that frees nothing");
		goto out;
	}
	btrfs_wib_add_sticky(fs_info, extra, BTRFS_WIB_BLOCK_SIZE);
	spin_lock_irqsave(&wib->lock, flags);
	e = find_live_entry(wib, extra);
	if (e)
		e->torn = e->sticky;
	spin_unlock_irqrestore(&wib->lock, flags);
	ret = btrfs_wib_mark(fs_info, base * BTRFS_WIB_ENTRY_SIZE, BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("mark into a recorded region failed: %d", ret);
		return ret;
	}
	start = jiffies;
	ret = btrfs_wib_mark(fs_info, fresh2, BTRFS_WIB_BLOCK_SIZE);
	waited = jiffies_to_msecs(jiffies - start);
	btrfs_wib_done(fs_info, base * BTRFS_WIB_ENTRY_SIZE, BTRFS_WIB_BLOCK_SIZE, false);
	if (!ret)
		btrfs_wib_done(fs_info, fresh2, BTRFS_WIB_BLOCK_SIZE, false);
	if (ret || waited >= 5000 || atomic64_read(&wib->stat_sticky_evicted) != evicted0 + 1) {
		test_err("with only a write in flight that frees nothing, the mark returned %d after %u ms, %llu record(s) spent",
			 ret, waited, atomic64_read(&wib->stat_sticky_evicted) - evicted0);
		return -EINVAL;
	}
out:
	btrfs_wib_clear_sticky(fs_info, extra, BTRFS_WIB_BLOCK_SIZE);
	for (u32 i = 0; i < nr - 1; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the write-in-flight test");
		return -EINVAL;
	}
	return 0;
}

/*
 * With every device there, a full log spends the possibly torn records a
 * failed flush left before the one the mount's recovery kept on a stripe it
 * could not decide (@kept_torn): that one refuses a read that would otherwise
 * return a rebuild from the parity alone where a sector does not read, now.
 * The recovery adds its records first, so in table order it went first.
 * raid56_wf_kept_torn_in_order=1 spends it first again.
 */
static int test_kept_torn_spent_last(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool unevictable = btrfs_wib_torn_unevictable() && !btrfs_wib_evicts_naming();
	const bool in_order = btrfs_wib_kept_torn_in_order() || btrfs_wib_evicts_naming();
	const u64 evicted0 = atomic64_read(&wib->stat_sticky_evicted);
	const u64 kept = 5000ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 fresh = 5001ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 base = 5010;
	const u32 nr = BTRFS_WIB_MAX_ENTRIES_V1;
	struct btrfs_wib_entry *e;
	unsigned long flags;
	u32 left = 0;
	int ret;

	clear_repairs(wib);
	btrfs_wib_add_sticky(fs_info, kept, BTRFS_WIB_BLOCK_SIZE);
	for (u32 i = 0; i < nr - 1; i++)
		btrfs_wib_add_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				     BTRFS_WIB_BLOCK_SIZE);
	spin_lock_irqsave(&wib->lock, flags);
	/* As btrfs_wib_recover() leaves a stripe it kept (wib_keep_torn()). */
	e = find_live_entry(wib, kept);
	if (e) {
		e->torn = e->sticky;
		e->kept_torn = e->sticky;
	}
	for (u32 i = 0; i < nr - 1; i++) {
		e = find_live_entry(wib, (base + i) * BTRFS_WIB_ENTRY_SIZE);
		if (e)
			e->torn = e->sticky;
	}
	e = find_live_entry(wib, kept);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (!e || e != &wib->entries[0] ||
	    !find_live_entry(wib, (base + nr - 2) * BTRFS_WIB_ENTRY_SIZE)) {
		test_err("the records were not created, the recovery's first");
		return -EINVAL;
	}

	/* Nothing in flight, nothing but possibly torn records. */
	ret = btrfs_wib_mark(fs_info, fresh, BTRFS_WIB_BLOCK_SIZE);
	if (unevictable) {
		if (ret != -EIO || !find_live_entry(wib, kept)) {
			test_err("torn records unevictable, yet the mark returned %d", ret);
			return -EINVAL;
		}
		test_msg("torn records unevictable: nothing spent, the write failed, as expected");
		goto out;
	}
	if (ret) {
		test_err("mark into a log full of possibly torn records failed: %d", ret);
		return ret;
	}
	btrfs_wib_done(fs_info, fresh, BTRFS_WIB_BLOCK_SIZE, false);
	for (u32 i = 0; i < nr - 1; i++)
		if (find_live_entry(wib, (base + i) * BTRFS_WIB_ENTRY_SIZE))
			left++;
	if (atomic64_read(&wib->stat_sticky_evicted) != evicted0 + 1) {
		test_err("expected one possibly torn record spent, saw %llu",
			 atomic64_read(&wib->stat_sticky_evicted) - evicted0);
		return -EINVAL;
	}
	if (in_order) {
		if (find_live_entry(wib, kept) || left != nr - 1) {
			test_err("spending in table order, yet the recovery's record is still there");
			return -EINVAL;
		}
		test_msg("torn records spent in table order: the recovery's first, as expected");
	} else if (!find_live_entry(wib, kept) || left != nr - 2) {
		test_err("the record the recovery kept was spent before those a failed flush left (%u of %u of theirs left)",
			 left, nr - 1);
		return -EINVAL;
	}
out:
	btrfs_wib_clear_sticky(fs_info, kept, BTRFS_WIB_BLOCK_SIZE);
	for (u32 i = 0; i < nr - 1; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the kept-torn test");
		return -EINVAL;
	}
	return 0;
}

/*
 * A device that did not confirm a flush may have lost what it acknowledged,
 * and no later flush brings that back.  So the stripes the last block lists
 * must stay on disk until the in-memory set has taken them back -- not only
 * through the commit whose flush failed, whose block is the union with the
 * last one anyway, but through the next one whose flush every device
 * confirms, which drops whatever the set does not list.
 *
 * The set here is too full to take them all back at the failed flush: writes
 * in flight whose record is not written yet hold the room.  The log used to
 * keep what fitted and let the next commit drop the rest.  Now it takes back
 * nothing yet and drops nothing until they are done -- there is no device
 * here to name, so nothing is left unnamed and nothing is lost, and no alert
 * is due.  With raid56_wf_no_readd_name=1 it still loses them, and this checks
 * that it does, so that a pass means the test can see the loss.
 *
 * Nor does a new write into a region neither the set nor the last block lists
 * take more of that room meanwhile: while such writes keep arriving the readd
 * never finds it.  It waits, and once the writes in flight are done, plans the
 * readd again itself -- no commit may come along to do it.  With
 * raid56_wf_readd_admits_new=1 it goes ahead, as it did.  Nor does a write into
 * a region the set holds only for the writes in flight there: those are what
 * the readd waits for, and while such writes overlap it waits for ever.  With
 * raid56_wf_readd_admits_busy=1 that one goes ahead, as it did.
 */
static int test_readd_keeps_records(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 unnamed0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_LOG_UNFLUSHED]);
	const u64 dropped0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]);
	const bool legacy = btrfs_wib_readd_legacy();
	const bool admits_new = btrfs_wib_readd_admits_new();
	const bool admits_busy = admits_new || btrfs_wib_readd_admits_busy();
	const u64 done_base = 3000;
	const u64 busy_base = 3200;
	const u64 fresh = 3300 * BTRFS_WIB_ENTRY_SIZE;
	const u32 nr = 100;
	unsigned long flags;
	void *saved;
	int ret = 0;

	saved = kmalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!saved)
		return -ENOMEM;

	/* Written, recorded on disk, finished: only the last block lists them. */
	for (u32 i = 0; i < nr; i++) {
		ret = btrfs_wib_mark(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE,
				     BTRFS_WIB_BLOCK_SIZE);
		if (ret) {
			test_err("mark %u failed: %d", i, ret);
			goto out;
		}
	}
	for (u32 i = 0; i < nr; i++)
		btrfs_wib_done(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE,
			       BTRFS_WIB_BLOCK_SIZE, false);
	if (block_nr_entries(wib->last) != nr) {
		test_err("the log lists %u regions, expected %u",
			 block_nr_entries(wib->last), nr);
		ret = -EINVAL;
		goto out;
	}
	memcpy(saved, wib->last, BTRFS_WIB_SLOT_SIZE);

	/* Writes in flight fill the set, with no commit of their own. */
	spin_lock_irqsave(&wib->lock, flags);
	for (u32 i = 0; i < nr && !ret; i++)
		ret = btrfs_wib_try_mark(wib, (busy_base + i) * BTRFS_WIB_ENTRY_SIZE,
					 BTRFS_WIB_BLOCK_SIZE);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret) {
		test_err("in-flight mark failed: %d", ret);
		goto out;
	}

	/* A device does not confirm the barrier. */
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, false);
	if (ret)
		goto out;
	if (btrfs_wib_block_drops(saved, wib->last)) {
		test_err("the commit whose flush failed dropped a region");
		ret = -EINVAL;
		goto out;
	}
	if (!legacy) {
		if (!wib->readd_owed) {
			test_err("the set could not take everything back, yet no readd is owed");
			ret = -EINVAL;
			goto out;
		}
		/* Nothing may stop being named while the readd is owed. */
		ret = btrfs_wib_persist_now(fs_info);
		if (ret != -EIO) {
			test_err("persist_now returned %d while a readd was owed, expected -EIO",
				 ret);
			ret = -EINVAL;
			goto out;
		}
		ret = -EINVAL;
		if (btrfs_wib_readd_refuses(wib, fresh, BTRFS_WIB_BLOCK_SIZE) == admits_new) {
			test_err("a write into a region nothing lists %s while the readd is owed",
				 admits_new ? "is refused" : "takes more of its room");
			goto out;
		}
		if (btrfs_wib_readd_refuses(wib, done_base * BTRFS_WIB_ENTRY_SIZE,
					    BTRFS_WIB_BLOCK_SIZE)) {
			test_err("a write into a region the last block lists is refused");
			goto out;
		}
		if (btrfs_wib_readd_refuses(wib, busy_base * BTRFS_WIB_ENTRY_SIZE,
					    BTRFS_WIB_BLOCK_SIZE) == admits_busy) {
			if (admits_busy)
				test_err("a write into a region the set holds is refused although raid56_wf_readd_admits_new or raid56_wf_readd_admits_busy is set");
			else
				test_err("a write into a region the readd waits to see go keeps it busy");
			goto out;
		}
		ret = 0;
	}

	/* The writes finish, and a write into a new region comes along. */
	for (u32 i = 0; i < nr; i++)
		btrfs_wib_done(fs_info, (busy_base + i) * BTRFS_WIB_ENTRY_SIZE,
			       BTRFS_WIB_BLOCK_SIZE, false);
	ret = btrfs_wib_mark(fs_info, fresh, BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("a write into a new region once the writes in flight finished failed: %d",
			 ret);
		goto out;
	}
	btrfs_wib_done(fs_info, fresh, BTRFS_WIB_BLOCK_SIZE, false);
	if (!legacy && wib->readd_owed != admits_new) {
		test_err("the readd is %s after the write waiting for it",
			 wib->readd_owed ? "still owed" : "no longer owed");
		ret = -EINVAL;
		goto out;
	}

	/* The next barrier every device confirms. */
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		goto out;

	if (legacy) {
		/* The negative control: the old readd loses what did not fit. */
		if (!btrfs_wib_block_drops(saved, wib->last)) {
			test_err("raid56_wf_no_readd_name is set, yet nothing was lost: the test is blind");
			ret = -EINVAL;
			goto out;
		}
		test_msg("raid56_wf_no_readd_name is set: records were lost, as expected");
	} else {
		if (btrfs_wib_block_drops(saved, wib->last)) {
			test_err("a region a failed flush kept was dropped by the next flushed commit");
			ret = -EINVAL;
			goto out;
		}
		if (wib->readd_owed) {
			test_err("the readd is still owed with room to take everything back");
			ret = -EINVAL;
			goto out;
		}
		for (u32 i = 0; i < nr; i++) {
			const struct btrfs_wib_entry *e =
				find_live_entry(wib, (done_base + i) * BTRFS_WIB_ENTRY_SIZE);

			if (!e || !(e->sticky & 0x1)) {
				test_err("region %u a failed flush kept is not recorded", i);
				ret = -EINVAL;
				goto out;
			}
		}
		if (atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_LOG_UNFLUSHED]) != unnamed0 ||
		    atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]) != dropped0) {
			test_err("an alert for a readd that neither lost a record nor left a device unnamed");
			ret = -EINVAL;
			goto out;
		}
	}

	for (u32 i = 0; i < nr; i++)
		btrfs_wib_clear_sticky(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE,
				       BTRFS_WIB_BLOCK_SIZE);
	/* Taken back with the rest where the write did not wait. */
	btrfs_wib_clear_sticky(fs_info, fresh, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		goto out;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the readd test");
		ret = -EINVAL;
		goto out;
	}
	ret = 0;
out:
	kfree(saved);
	return ret;
}

/*
 * An owed readd whose room writes in flight hold for BTRFS_WIB_READD_WAIT --
 * stuck on a device, since no new write takes any more of it -- stops waiting
 * for them: it takes back what fits and loses the rest, loudly, rather than
 * fail every recorded write for as long as they are stuck.  With
 * raid56_wf_readd_admits_new=1 it waits on, as it did.
 */
static int test_readd_wait_bounded(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 dropped0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]);
	const bool admits_new = btrfs_wib_readd_admits_new();
	const u64 done_base = 3400;
	const u64 busy_base = 3600;
	const u32 nr = 100;
	unsigned long flags;
	u32 kept = 0;
	int ret = 0;

	if (btrfs_wib_readd_legacy()) {
		test_msg("raid56_wf_no_readd_name is set, skipping the bounded readd test");
		return 0;
	}
	for (u32 i = 0; i < nr; i++) {
		ret = btrfs_wib_mark(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE,
				     BTRFS_WIB_BLOCK_SIZE);
		if (ret) {
			test_err("mark %u failed: %d", i, ret);
			return ret;
		}
		btrfs_wib_done(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE,
			       BTRFS_WIB_BLOCK_SIZE, false);
	}
	spin_lock_irqsave(&wib->lock, flags);
	for (u32 i = 0; i < nr && !ret; i++)
		ret = btrfs_wib_try_mark(wib, (busy_base + i) * BTRFS_WIB_ENTRY_SIZE,
					 BTRFS_WIB_BLOCK_SIZE);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret) {
		test_err("in-flight mark failed: %d", ret);
		return ret;
	}
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, false);
	if (ret)
		return ret;
	if (!wib->readd_owed) {
		test_err("the set could not take everything back, yet no readd is owed");
		return -EINVAL;
	}

	/* The writes in flight are stuck: it has waited that long already. */
	spin_lock_irqsave(&wib->lock, flags);
	wib->readd_until = jiffies - 1;
	spin_unlock_irqrestore(&wib->lock, flags);
	ret = btrfs_wib_persist_now(fs_info);
	for (u32 i = 0; i < nr; i++) {
		const struct btrfs_wib_entry *e =
			find_live_entry(wib, (done_base + i) * BTRFS_WIB_ENTRY_SIZE);

		if (e && (e->sticky & 0x1))
			kept++;
	}
	if (admits_new) {
		if (ret != -EIO || !wib->readd_owed || kept ||
		    atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]) != dropped0) {
			test_err("raid56_wf_readd_admits_new is set, yet the readd stopped waiting: persist %d owed %d kept %u",
				 ret, wib->readd_owed, kept);
			return -EINVAL;
		}
		test_msg("raid56_wf_readd_admits_new is set: the readd waits on, as expected");
	} else {
		if (ret || wib->readd_owed) {
			test_err("the readd still waits past its bound: persist %d owed %d",
				 ret, wib->readd_owed);
			return -EINVAL;
		}
		if (!kept || kept == nr) {
			test_err("the readd that stopped waiting kept %u of %u regions, expected what fits",
				 kept, nr);
			return -EINVAL;
		}
		if (atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]) != dropped0 + 1) {
			test_err("records were lost, and nothing said so");
			return -EINVAL;
		}
	}

	for (u32 i = 0; i < nr; i++) {
		btrfs_wib_done(fs_info, (busy_base + i) * BTRFS_WIB_ENTRY_SIZE,
			       BTRFS_WIB_BLOCK_SIZE, false);
		btrfs_wib_clear_sticky(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE,
				       BTRFS_WIB_BLOCK_SIZE);
	}
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	/* Under the knob the commit took them back first. */
	for (u32 i = 0; i < nr; i++)
		btrfs_wib_clear_sticky(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE,
				       BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (wib->readd_owed || block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the bounded readd test: owed %d, %u entries",
			 wib->readd_owed, block_nr_entries(wib->last));
		return -EINVAL;
	}
	return 0;
}

/*
 * Writers that keep a region busy -- two writes in flight there, one starting
 * before the other finishes -- while an owed readd waits for that region: it
 * is not listed by the last block, which is full.  A write into it there is
 * refused as one into a new region is, so the region drains as its writes do,
 * and the readd takes everything back.  Admitted, it kept the region, and
 * with it the readd, waiting until BTRFS_WIB_READD_WAIT ran out -- every
 * recorded write failing meanwhile -- and then lost what did not fit.  So it
 * still does under raid56_wf_readd_admits_busy=1, which this checks, so that a
 * pass means the test can see it.
 */
static int test_readd_busy_region(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 dropped0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]);
	const bool admits = btrfs_wib_readd_admits_busy();
	const u64 done_base = 4400;
	const u32 nr = BTRFS_WIB_MAX_ENTRIES_V1;
	const u64 busy = (done_base + nr) * BTRFS_WIB_ENTRY_SIZE;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	unsigned long flags;
	bool second = false;
	u32 kept = 0;
	int ret = 0;

	if (btrfs_wib_readd_legacy() || btrfs_wib_readd_admits_new()) {
		test_msg("raid56_wf_no_readd_name or raid56_wf_readd_admits_new is set, skipping the busy region test");
		return 0;
	}
	/* Written and finished: the last block lists as many as it holds. */
	for (u32 i = 0; i < nr; i++) {
		ret = btrfs_wib_mark(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE, blk);
		if (ret) {
			test_err("mark %u failed: %d", i, ret);
			return ret;
		}
		btrfs_wib_done(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE, blk, false);
	}
	if (block_nr_entries(wib->last) != nr) {
		test_err("the log lists %u regions, expected %u", block_nr_entries(wib->last), nr);
		return -EINVAL;
	}

	/* A write in flight into one more region, and the flush fails. */
	spin_lock_irqsave(&wib->lock, flags);
	ret = btrfs_wib_try_mark(wib, busy, blk);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret) {
		test_err("in-flight mark failed: %d", ret);
		return ret;
	}
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, false);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (!wib->readd_owed) {
		test_err("the set could not take everything back, yet no readd is owed");
		goto out;
	}

	/* A second writer into the same region, before the first finishes. */
	if (!btrfs_wib_readd_refuses(wib, busy + blk, blk)) {
		spin_lock_irqsave(&wib->lock, flags);
		second = !btrfs_wib_try_mark(wib, busy + blk, blk);
		spin_unlock_irqrestore(&wib->lock, flags);
	}
	if (second != admits) {
		if (admits)
			test_err("raid56_wf_readd_admits_busy is set, yet the second write was refused: the test is blind");
		else
			test_err("a write into the region the readd waits for was recorded");
		goto out;
	}
	btrfs_wib_done(fs_info, busy, blk, false);
	/* A refused write plans the readd again (wib_readd_settle()). */
	ret = btrfs_wib_persist_now(fs_info);
	if (!admits && (ret || wib->readd_owed)) {
		test_err("the readd still waits once the region drained: persist %d owed %d",
			 ret, wib->readd_owed);
		ret = -EINVAL;
		goto out;
	}
	if (admits && (ret != -EIO || !wib->readd_owed)) {
		test_err("raid56_wf_readd_admits_busy is set, yet the readd stopped waiting: persist %d owed %d",
			 ret, wib->readd_owed);
		ret = -EINVAL;
		goto out;
	}
	/* Its bound runs out. */
	spin_lock_irqsave(&wib->lock, flags);
	wib->readd_until = jiffies - 1;
	spin_unlock_irqrestore(&wib->lock, flags);
	ret = btrfs_wib_persist_now(fs_info);
	if (ret || wib->readd_owed) {
		test_err("the readd still waits past its bound: persist %d owed %d", ret,
			 wib->readd_owed);
		ret = -EINVAL;
		goto out;
	}
	for (u32 i = 0; i < nr; i++) {
		const struct btrfs_wib_entry *e =
			find_live_entry(wib, (done_base + i) * BTRFS_WIB_ENTRY_SIZE);

		if (e && (e->sticky & 0x1))
			kept++;
	}
	ret = -EINVAL;
	if (admits) {
		if (kept == nr ||
		    atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]) == dropped0) {
			test_err("raid56_wf_readd_admits_busy is set, yet nothing was lost (kept %u of %u): the test is blind",
				 kept, nr);
			goto out;
		}
		test_msg("raid56_wf_readd_admits_busy is set: the busy region held the readd until it lost records, as expected");
	} else if (kept != nr ||
		   atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]) != dropped0) {
		test_err("the readd lost records (kept %u of %u) although the busy region drained",
			 kept, nr);
		goto out;
	}
	ret = 0;
out:
	if (second)
		btrfs_wib_done(fs_info, busy + blk, blk, false);
	btrfs_wib_done(fs_info, busy, blk, false);
	for (int pass = 0; pass < 2; pass++) {
		for (u32 i = 0; i <= nr; i++)
			btrfs_wib_clear_sticky(fs_info, (done_base + i) * BTRFS_WIB_ENTRY_SIZE,
					       BTRFS_WIB_ENTRY_SIZE);
		btrfs_wib_commit_prepare(fs_info);
		if (btrfs_wib_commit(fs_info, true) && !ret)
			ret = -EIO;
	}
	if (!ret && (wib->readd_owed || block_nr_entries(wib->last) != 0)) {
		test_err("log not empty after the busy region test: owed %d, %u entries",
			 wib->readd_owed, block_nr_entries(wib->last));
		ret = -EINVAL;
	}
	return ret;
}

/*
 * A failed flush takes back what the last block lists and the set no longer
 * holds -- here the block of a write that finished -- and with it the stale
 * marks the last block carried for the same region.  Those the set still holds
 * are the same marks, and stay what they were: the recovery's verdict on a
 * stripe it could not decide (@suspect_par, and @prior_par for one an earlier
 * mount wrote), and a running replace's record of the zeros on its target.
 * Taken over by the readd, each became a plain stale record, which a full log
 * with a device missing spends before them.  Under raid56_wf_readd_disowns_all=1
 * (or raid56_wf_no_readd_name=1, the readd from before the names) they are
 * taken over, and this checks that they are, so that a pass means the test can
 * see it.
 */
static int test_readd_keeps_verdicts(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool disowns = btrfs_wib_readd_disowns_all() || btrfs_wib_readd_legacy();
	const bool prior_kept = !btrfs_wib_reload_verdicts_plain();
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	const u64 suspect = 4200ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 prior = 4201ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 owned = 4202ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 regions[] = { suspect, prior, owned };
	const struct btrfs_wib_entry *e;
	struct btrfs_wib_entry old = { 0 };
	bool carried = false;
	bool kept;
	int ret = -EINVAL;

	/* A stripe of three data columns at block 0; the replace's at block 1. */
	btrfs_wib_add_sticky(fs_info, suspect, 3 * blk);
	btrfs_wib_mark_suspect_parity(fs_info, suspect, 3 * blk, 0);
	btrfs_wib_add_sticky(fs_info, prior, 3 * blk);
	btrfs_wib_update_stale_parity(fs_info, prior, 0, true);
	btrfs_wib_keep_prior_verdict(fs_info, prior, BIT(0));
	btrfs_wib_add_sticky(fs_info, owned, 2 * blk);
	if (!btrfs_wib_replace_mark_stale(fs_info, owned + blk, true)) {
		test_err("the replace's mark was not kept");
		goto out;
	}

	/*
	 * A write into another stripe of each region, recorded by a block that
	 * carries the marks (wide: the prior verdict and the replace's mark
	 * count as stale), then finished: the last block lists it in flight,
	 * the set no longer holds it.
	 */
	for (int i = 0; i < ARRAY_SIZE(regions); i++) {
		ret = btrfs_wib_mark(fs_info, regions[i] + 8 * blk, blk);
		if (ret) {
			test_err("mark in region %d failed: %d", i, ret);
			goto out;
		}
		btrfs_wib_done(fs_info, regions[i] + 8 * blk, blk, false);
	}
	ret = -EINVAL;
	for (u32 i = 0; i < block_nr_entries(wib->last); i++) {
		btrfs_wib_read_entry(wib->last, i, &old);
		if (old.bytenr == suspect && (old.stale_par & BIT(0)))
			carried = true;
	}
	if (!carried) {
		/* raid56_stale_no_persist=1: nothing to carry back. */
		test_msg("the last block carries no stale parity, skipping the readd verdict test");
		ret = 0;
		goto out;
	}

	/* A device does not confirm the barrier. */
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, false);
	if (ret)
		goto out;
	ret = -EINVAL;
	for (int i = 0; i < ARRAY_SIZE(regions); i++) {
		e = find_live_entry(wib, regions[i]);
		if (!e || !(e->sticky & BIT(8))) {
			test_err("the finished write in region %d was not taken back", i);
			goto out;
		}
	}
	e = find_live_entry(wib, suspect);
	kept = e->suspect_par & BIT(0);
	e = find_live_entry(wib, prior);
	if (prior_kept && !(e->prior_par & BIT(0)) != !kept) {
		test_err("the readd kept one verdict and not the other");
		goto out;
	}
	e = find_live_entry(wib, owned);
	if (!(e->replace_stale & BIT(1)) != !kept) {
		test_err("the readd kept the verdicts %s the replace's mark",
			 kept ? "but not" : "and");
		goto out;
	}
	if (kept == disowns) {
		if (disowns)
			test_err("raid56_wf_readd_disowns_all or raid56_wf_no_readd_name is set, yet the readd kept the verdicts: the test is blind");
		else
			test_err("a failed flush's readd took the recovery's verdicts and a replace's mark for its own");
		goto out;
	}
	if (disowns)
		test_msg("raid56_wf_readd_disowns_all or raid56_wf_no_readd_name is set: the readd took the verdicts over, as expected");
	ret = 0;
out:
	btrfs_wib_replace_end(fs_info, false);
	for (int i = 0; i < ARRAY_SIZE(regions); i++)
		btrfs_wib_clear_sticky(fs_info, regions[i], BTRFS_WIB_ENTRY_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	if (btrfs_wib_commit(fs_info, true) && !ret)
		ret = -EIO;
	if (!ret && (wib->readd_owed || block_nr_entries(wib->last) != 0 ||
		     btrfs_wib_any_stale(fs_info))) {
		test_err("log not empty after the readd verdict test: owed %d, %u entries",
			 wib->readd_owed, block_nr_entries(wib->last));
		ret = -EINVAL;
	}
	return ret;
}

/*
 * The readd with something to name: a RAID5 chunk on three devices, so that
 * wib_name_devices() finds each device's column and parity through the chunk
 * map the way it does on a mounted filesystem.  Without one no name is ever
 * produced, and neither is any of the states below.
 */
#define READD_CHUNK		(64ULL * SZ_1G)
#define READD_NR_DEVS		3
#define READD_NR_DATA		2
#define READD_FAILED		1
#define READD_STRIPE_LEN	(READD_NR_DATA * BTRFS_WIB_BLOCK_SIZE)

struct readd_rig {
	struct btrfs_fs_info *fs_info;
	struct btrfs_device *devs[READD_NR_DEVS];
	struct btrfs_chunk_map *map;
};

/* The full stripe that starts region @k of the chunk. */
static u64 readd_region(u64 k)
{
	return READD_CHUNK + k * BTRFS_WIB_ENTRY_SIZE;
}

/*
 * The names a flush that failed on device READD_FAILED puts on the full
 * stripe at the start of region @k: the block of its column, or parity bit 0
 * if the parity is there.  Column c of full stripe r is on stripes[(c + r) %
 * num_stripes] and the parity follows the data columns, as btrfs_map_block()
 * lays them out.
 */
static void readd_expect(u64 k, u64 *stale, u64 *stale_par)
{
	const u64 r = k * (BTRFS_WIB_ENTRY_SIZE / READD_STRIPE_LEN);

	*stale = 0;
	*stale_par = 0;
	for (int c = 0; c < READD_NR_DATA; c++)
		if ((c + r) % READD_NR_DEVS == READD_FAILED)
			*stale |= BIT_ULL(c);
	if ((READD_NR_DATA + r) % READD_NR_DEVS == READD_FAILED)
		*stale_par |= BIT_ULL(0);
}

/* Commit with the barrier failing on device @idx, as write_all_supers() leaves it. */
static int readd_commit_failed_dev(struct readd_rig *rig, int idx)
{
	struct btrfs_device *dev = rig->devs[idx];
	int ret;

	set_bit(BTRFS_DEV_STATE_FLUSH_FAILED, &dev->dev_state);
	btrfs_wib_commit_prepare(rig->fs_info);
	ret = btrfs_wib_commit(rig->fs_info, false);
	clear_bit(BTRFS_DEV_STATE_FLUSH_FAILED, &dev->dev_state);
	return ret;
}

static int readd_commit_failed(struct readd_rig *rig)
{
	return readd_commit_failed_dev(rig, READD_FAILED);
}

/*
 * The write into the full stripe at the start of region @k issues its bios, as
 * rmw_rbio() tells the log before phase B: to data columns @cols and parity
 * @par.
 */
static void readd_issue(struct readd_rig *rig, u64 k, u64 cols, u32 par)
{
	btrfs_wib_note_written(rig->fs_info, readd_region(k), READD_NR_DATA, cols, par,
			       true);
}

static int readd_commit(struct readd_rig *rig)
{
	btrfs_wib_commit_prepare(rig->fs_info);
	return btrfs_wib_commit(rig->fs_info, true);
}

/* Written, recorded on disk and finished: regions [@first, @first + @nr). */
static int readd_write_done(struct readd_rig *rig, u64 first, u64 nr)
{
	for (u64 k = first; k < first + nr; k++) {
		int ret = btrfs_wib_mark(rig->fs_info, readd_region(k), READD_STRIPE_LEN);

		if (ret) {
			test_err("mark of region %llu failed: %d", k, ret);
			return ret;
		}
		readd_issue(rig, k, 0x3, 0x1);
		btrfs_wib_done(rig->fs_info, readd_region(k), READD_STRIPE_LEN, false);
	}
	return 0;
}

/* Retire every record in regions [@first, @first + @nr) and empty the log. */
static int readd_clear(struct readd_rig *rig, u64 first, u64 nr)
{
	struct btrfs_wib *wib = rig->fs_info->wib;
	int ret;

	for (u64 k = first; k < first + nr; k++)
		btrfs_wib_clear_sticky(rig->fs_info, readd_region(k), READD_STRIPE_LEN);
	ret = readd_commit(rig);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0 || btrfs_wib_any_stale(rig->fs_info)) {
		test_err("log not empty after the readd names test: %u entries, stale %d",
			 block_nr_entries(wib->last), btrfs_wib_any_stale(rig->fs_info));
		return -EINVAL;
	}
	return 0;
}

/*
 * Which blocks the failed flush names: the ones it was going to drop, and the
 * ones with a write in flight -- whose own completion must then not clear the
 * name -- but not a record some earlier failure made and nothing has written
 * since.
 */
static int readd_names_which(struct readd_rig *rig, bool legacy)
{
	struct btrfs_fs_info *fs_info = rig->fs_info;
	struct btrfs_wib *wib = fs_info->wib;
	/* Column 1 on the failed device; column 0 recorded stale long ago. */
	const u64 k_old = 3;
	/* Finished: parity on the failed device. */
	const u64 k_done = 1;
	/* In flight: column 1, and parity, on the failed device. */
	const u64 k_fly = 0;
	const u64 k_fly_par = 4;
	const struct btrfs_wib_entry *e;
	u64 stale, stale_par;
	unsigned long flags;
	int ret;

	readd_expect(k_old, &stale, &stale_par);
	if (stale != BIT_ULL(1)) {
		test_err("the rig does not put column 1 of region %llu on the failed device",
			 k_old);
		return -EINVAL;
	}
	btrfs_wib_add_sticky(fs_info, readd_region(k_old), READD_STRIPE_LEN);
	btrfs_wib_mark_stale(fs_info, readd_region(k_old), BTRFS_WIB_BLOCK_SIZE);
	ret = readd_commit(rig);
	if (ret)
		return ret;
	ret = readd_write_done(rig, k_done, 1);
	if (ret)
		return ret;
	ret = btrfs_wib_mark(fs_info, readd_region(k_fly), READD_STRIPE_LEN);
	if (!ret)
		ret = btrfs_wib_mark(fs_info, readd_region(k_fly_par), READD_STRIPE_LEN);
	if (ret) {
		test_err("in-flight mark failed: %d", ret);
		return ret;
	}
	/* Both have issued their writes: they are in the devices' caches. */
	readd_issue(rig, k_fly, 0x3, 0x1);
	readd_issue(rig, k_fly_par, 0x3, 0x1);

	ret = readd_commit_failed(rig);
	if (ret)
		return ret;

	e = find_live_entry(wib, readd_region(k_done));
	readd_expect(k_done, &stale, &stale_par);
	if (!e || e->sticky != 0x3) {
		test_err("the finished stripe a failed flush kept is not recorded");
		return -EINVAL;
	}
	if (!legacy && (e->stale != stale || e->stale_par != stale_par)) {
		test_err("finished stripe named stale 0x%llx stale_par 0x%llx, expected 0x%llx 0x%llx",
			 e->stale, e->stale_par, stale, stale_par);
		return -EINVAL;
	}
	if (legacy && (e->stale | e->stale_par)) {
		test_err("raid56_wf_no_readd_name is set, yet the finished stripe is named");
		return -EINVAL;
	}

	e = find_live_entry(wib, readd_region(k_fly));
	readd_expect(k_fly, &stale, &stale_par);
	if (!e || e->bitmap != 0x3) {
		test_err("the stripe in flight lost its in-flight bits");
		return -EINVAL;
	}
	if (!legacy && (e->sticky != 0x3 || e->stale != stale || e->hold != stale)) {
		test_err("stripe in flight: sticky 0x%llx stale 0x%llx hold 0x%llx, expected 0x3 0x%llx 0x%llx",
			 e->sticky, e->stale, e->hold, stale, stale);
		return -EINVAL;
	}
	if (legacy && (e->sticky | e->stale)) {
		test_err("raid56_wf_no_readd_name is set, yet the stripe in flight is named");
		return -EINVAL;
	}
	e = find_live_entry(wib, readd_region(k_fly_par));
	readd_expect(k_fly_par, &stale, &stale_par);
	if (!legacy && (!e || e->stale_par != stale_par || e->hold_par != stale_par ||
			!stale_par)) {
		test_err("the parity of the stripe in flight is not named and held");
		return -EINVAL;
	}

	/* An old record is not a new write: no second stale column for it. */
	e = find_live_entry(wib, readd_region(k_old));
	if (!e || e->stale != BIT_ULL(0) || e->stale_par) {
		test_err("the old record now reads stale 0x%llx stale_par 0x%llx, expected 0x1 0x0",
			 e ? e->stale : 0, e ? e->stale_par : 0);
		return -EINVAL;
	}
	if (wib->readd_owed) {
		test_err("a readd with room to spare is owed");
		return -EINVAL;
	}

	/*
	 * The block written after the failed flush lists the finished stripe
	 * as the error record the readd made of it, no longer in flight: its
	 * write ended before the flush was issued, the name says what the
	 * flush may have lost, and in flight it would read to a mount after a
	 * crash as a write that may have torn the stripe (wib_readd_base()).
	 * The stripe still in flight stays in flight.  Under
	 * raid56_wf_finished_stay_inflight=1, and without names, the finished
	 * stripe stays listed in flight as it did.
	 */
	{
		const u64 fly = legacy || btrfs_wib_finished_stay_inflight() ? 0x3 : 0;

		if (block_bitmap(wib->last, readd_region(k_done)) != fly ||
		    ((block_bitmap(wib->last, readd_region(k_done)) |
		      block_error(wib->last, readd_region(k_done))) & 0x3) != 0x3) {
			test_err("the block after the failed flush lists the finished stripe in flight 0x%llx error 0x%llx, expected in flight 0x%llx and listed",
				 block_bitmap(wib->last, readd_region(k_done)),
				 block_error(wib->last, readd_region(k_done)), fly);
			return -EINVAL;
		}
		if (block_bitmap(wib->last, readd_region(k_fly)) != 0x3) {
			test_err("the block after the failed flush lists the stripe in flight as 0x%llx, expected 0x3",
				 block_bitmap(wib->last, readd_region(k_fly)));
			return -EINVAL;
		}
		if (fly)
			test_msg("%s is set: the finished stripe stays in flight, as expected",
				 legacy ? "raid56_wf_no_readd_name" :
					  "raid56_wf_finished_stay_inflight");
	}

	/*
	 * The writes in flight complete, as rmw_rbio() finishes one: done,
	 * then their columns and parity cleared because they landed.  Their
	 * landing is exactly what the failed flush put in doubt.
	 */
	btrfs_wib_done(fs_info, readd_region(k_fly), READD_STRIPE_LEN, false);
	btrfs_wib_done(fs_info, readd_region(k_fly_par), READD_STRIPE_LEN, false);
	btrfs_wib_clear_stale(fs_info, readd_region(k_fly), READD_STRIPE_LEN, false);
	btrfs_wib_update_stale_parity(fs_info, readd_region(k_fly_par), 0, false);
	if (!legacy) {
		readd_expect(k_fly, &stale, &stale_par);
		e = find_live_entry(wib, readd_region(k_fly));
		if (!e || e->stale != stale) {
			test_err("the write in flight cleared the name the failed flush put on it");
			return -EINVAL;
		}
		readd_expect(k_fly_par, &stale, &stale_par);
		e = find_live_entry(wib, readd_region(k_fly_par));
		if (!e || e->stale_par != stale_par) {
			test_err("the write in flight cleared the parity name the failed flush put on it");
			return -EINVAL;
		}

		/* A write-back with FUA may clear it. */
		btrfs_wib_clear_stale(fs_info, readd_region(k_fly), READD_STRIPE_LEN, true);
		e = find_live_entry(wib, readd_region(k_fly));
		if (!e || e->stale || e->hold) {
			test_err("a durable write-back did not clear the held name");
			return -EINVAL;
		}
		/* So may a write that starts after the failed flush. */
		spin_lock_irqsave(&wib->lock, flags);
		ret = btrfs_wib_try_mark(wib, readd_region(k_fly_par), READD_STRIPE_LEN);
		spin_unlock_irqrestore(&wib->lock, flags);
		if (ret) {
			test_err("new write into the held stripe failed to mark: %d", ret);
			return ret;
		}
		btrfs_wib_done(fs_info, readd_region(k_fly_par), READD_STRIPE_LEN, false);
		btrfs_wib_update_stale_parity(fs_info, readd_region(k_fly_par), 0, false);
		e = find_live_entry(wib, readd_region(k_fly_par));
		if (!e || e->stale_par || e->hold_par) {
			test_err("a write after the failed flush could not clear the parity name");
			return -EINVAL;
		}
	}

	return readd_clear(rig, 0, k_fly_par + 1);
}

/*
 * A possibly torn mark the set has spent, as a full log spends a record that
 * says nothing more (wib_entry_torn_only()), is still in the last block: in
 * flight and as an error record, which is how the mark is written.  A failed
 * flush takes it back as an error record, and the block written after it must
 * go on listing it in flight -- nothing says it was a write that finished --
 * while a write that did finish before the flush is listed as the error record
 * the readd made of it (wib_readd_base()), unless raid56_wf_no_readd_name or
 * raid56_wf_finished_stay_inflight is set.
 */
static int readd_names_spent_torn(struct readd_rig *rig, bool legacy)
{
	struct btrfs_fs_info *fs_info = rig->fs_info;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 k_torn = 520;
	const u64 k_done = 521;
	const u64 fly = legacy || btrfs_wib_finished_stay_inflight() ? 0x3 : 0;
	struct btrfs_wib_entry *e;
	int ret;

	btrfs_wib_add_sticky(fs_info, readd_region(k_torn), READD_STRIPE_LEN);
	e = find_live_entry(wib, readd_region(k_torn));
	if (!e) {
		test_err("the possibly torn record is not in the set");
		return -EINVAL;
	}
	e->torn = e->sticky;
	ret = readd_commit(rig);
	if (ret)
		return ret;
	if (btrfs_wib_torn_no_persist())
		return readd_clear(rig, k_torn, 1);
	if (block_bitmap(wib->last, readd_region(k_torn)) != 0x3 ||
	    block_error(wib->last, readd_region(k_torn)) != 0x3) {
		test_err("the possibly torn record is listed in flight 0x%llx error 0x%llx, expected 0x3 0x3",
			 block_bitmap(wib->last, readd_region(k_torn)),
			 block_error(wib->last, readd_region(k_torn)));
		return -EINVAL;
	}
	btrfs_wib_clear_sticky(fs_info, readd_region(k_torn), READD_STRIPE_LEN);
	ret = readd_write_done(rig, k_done, 1);
	if (ret)
		return ret;

	ret = readd_commit_failed(rig);
	if (ret)
		return ret;
	if (block_bitmap(wib->last, readd_region(k_torn)) != 0x3) {
		test_err("the block after the failed flush lists the spent possibly torn record in flight as 0x%llx, expected 0x3",
			 block_bitmap(wib->last, readd_region(k_torn)));
		return -EINVAL;
	}
	if (block_bitmap(wib->last, readd_region(k_done)) != fly ||
	    (block_error(wib->last, readd_region(k_done)) | fly) != 0x3) {
		test_err("the block after the failed flush lists the finished stripe in flight 0x%llx error 0x%llx, expected in flight 0x%llx",
			 block_bitmap(wib->last, readd_region(k_done)),
			 block_error(wib->last, readd_region(k_done)), fly);
		return -EINVAL;
	}
	return readd_clear(rig, k_torn, 2);
}

/*
 * A failed flush names a member only where a write went to it since its device
 * last confirmed a flush -- the one thing a lost cache can have taken.
 *
 *  - k_cache is uml/rmw_cache.sh: B's write failed on its own device (column
 *    0, recorded stale) while its parity went to the parity device's cache;
 *    B's device then failed the barrier, which the parity device confirmed.
 *    C's write into the stripe is in flight, its phase B not issued yet, when
 *    the parity device fails the next flush.  Nothing went to that parity
 *    since it was flushed, so it is not named: named, the stripe would have
 *    two stale members under one parity, and the read of B, which the parity
 *    rebuilds exactly, would be refused.
 *  - k_settled: a record from an earlier failure (column 0) that a later
 *    write into column 1, on the failing device, went to since.  That write
 *    is lost like any other: named, although the record is not new.
 *  - k_flushed: the same, but a flush every device confirmed came between the
 *    write and the failure.  Not named.
 *  - k_repair: a record whose column on the failing device a repair wrote
 *    back without FUA (btrfs_wib_note_written_data(), a read repair or a
 *    scrub's), no read-modify-write involved.  Named.
 *
 * With raid56_wf_name_unwritten=1 the readd names as it did before: every
 * block with a write in flight, never a record already there.
 */
static int readd_names_written(struct readd_rig *rig, bool legacy)
{
	struct btrfs_fs_info *fs_info = rig->fs_info;
	struct btrfs_wib *wib = fs_info->wib;
	const bool unwritten = btrfs_wib_name_unwritten();
	/* Column 0 on device 2, column 1 on device 0, parity on the failed one. */
	const u64 k_cache = 502;
	/* Column 1 on the failed device, the parity on device 2. */
	const u64 k_settled = 501;
	/* Column 0 on the failed device, both. */
	const u64 k_flushed = 500;
	const u64 k_repair = 503;
	const struct btrfs_wib_entry *e;
	u64 stale, stale_par;
	int ret;

	readd_expect(k_cache, &stale, &stale_par);
	if (stale || stale_par != 0x1) {
		test_err("the rig does not put only the parity of region %llu on the failed device",
			 k_cache);
		return -EINVAL;
	}
	readd_expect(k_settled, &stale, &stale_par);
	if (stale != 0x2 || stale_par) {
		test_err("the rig does not put column 1 of region %llu on the failed device",
			 k_settled);
		return -EINVAL;
	}
	readd_expect(k_flushed, &stale, &stale_par);
	if (stale != 0x1 || stale_par) {
		test_err("the rig does not put column 0 of region %llu on the failed device",
			 k_flushed);
		return -EINVAL;
	}
	readd_expect(k_repair, &stale, &stale_par);
	if (stale != 0x1 || stale_par) {
		test_err("the rig does not put column 0 of region %llu on the failed device",
			 k_repair);
		return -EINVAL;
	}

	/* k_flushed: an old record, a write to the failed device's column, a good flush. */
	btrfs_wib_add_sticky(fs_info, readd_region(k_flushed), READD_STRIPE_LEN);
	btrfs_wib_mark_stale(fs_info, readd_region(k_flushed) + BTRFS_WIB_BLOCK_SIZE,
			     BTRFS_WIB_BLOCK_SIZE);
	ret = btrfs_wib_mark(fs_info, readd_region(k_flushed), READD_STRIPE_LEN);
	if (ret)
		goto mark_failed;
	readd_issue(rig, k_flushed, 0x1, 0x1);
	btrfs_wib_done(fs_info, readd_region(k_flushed), READD_STRIPE_LEN, false);
	ret = readd_commit(rig);
	if (ret)
		return ret;

	/* k_cache: B's write fails on device 2, whose barrier fails too. */
	ret = btrfs_wib_mark(fs_info, readd_region(k_cache), READD_STRIPE_LEN);
	if (ret)
		goto mark_failed;
	readd_issue(rig, k_cache, 0x1, 0x1);
	btrfs_wib_done(fs_info, readd_region(k_cache), READD_STRIPE_LEN, true);
	btrfs_wib_mark_stale(fs_info, readd_region(k_cache), BTRFS_WIB_BLOCK_SIZE);
	ret = readd_commit_failed_dev(rig, 2);
	if (ret)
		return ret;

	/* k_settled: an old record, then a write to the failed device's column. */
	btrfs_wib_add_sticky(fs_info, readd_region(k_settled), READD_STRIPE_LEN);
	btrfs_wib_mark_stale(fs_info, readd_region(k_settled), BTRFS_WIB_BLOCK_SIZE);
	ret = btrfs_wib_mark(fs_info, readd_region(k_settled), READD_STRIPE_LEN);
	if (ret)
		goto mark_failed;
	readd_issue(rig, k_settled, 0x2, 0x1);
	btrfs_wib_done(fs_info, readd_region(k_settled), READD_STRIPE_LEN, false);

	/* k_repair: a record, then a repair writes its column 0 back. */
	btrfs_wib_add_sticky(fs_info, readd_region(k_repair), READD_STRIPE_LEN);
	btrfs_wib_note_written_data(fs_info, readd_region(k_repair) + SZ_4K, SZ_4K);

	/* C: in flight into k_cache, nothing issued, when the parity device fails. */
	ret = btrfs_wib_mark(fs_info, readd_region(k_cache), READD_STRIPE_LEN);
	if (ret)
		goto mark_failed;
	ret = readd_commit_failed(rig);
	if (ret)
		return ret;

	ret = -EINVAL;
	e = find_live_entry(wib, readd_region(k_cache));
	if (!e || e->bitmap != 0x3 || e->stale != 0x1) {
		test_err("region %llu: bitmap 0x%llx stale 0x%llx, expected 0x3 0x1",
			 k_cache, e ? e->bitmap : 0, e ? e->stale : 0);
		goto out;
	}
	if (!legacy && unwritten && e->stale_par != 0x1) {
		test_err("raid56_wf_name_unwritten is set, yet the parity of the stripe in flight is not named");
		goto out;
	}
	if ((legacy || !unwritten) && e->stale_par) {
		test_err("a parity nothing wrote to since its device confirmed a flush is named stale");
		goto out;
	}
	e = find_live_entry(wib, readd_region(k_settled));
	stale = !legacy && !unwritten ? 0x3 : 0x1;
	if (!e || e->stale != stale || e->stale_par) {
		test_err("region %llu: stale 0x%llx stale_par 0x%llx, expected 0x%llx 0x0",
			 k_settled, e ? e->stale : 0, e ? e->stale_par : 0, stale);
		goto out;
	}
	e = find_live_entry(wib, readd_region(k_flushed));
	if (!e || e->stale != 0x2 || e->stale_par) {
		test_err("region %llu: stale 0x%llx stale_par 0x%llx, expected 0x2 0x0: a write a flush covered was named",
			 k_flushed, e ? e->stale : 0, e ? e->stale_par : 0);
		goto out;
	}
	e = find_live_entry(wib, readd_region(k_repair));
	stale = !legacy && !unwritten ? 0x1 : 0;
	if (!e || e->stale != stale || e->stale_par) {
		test_err("region %llu: stale 0x%llx stale_par 0x%llx, expected 0x%llx 0x0: a repair's write-back",
			 k_repair, e ? e->stale : 0, e ? e->stale_par : 0, stale);
		goto out;
	}
	btrfs_wib_done(fs_info, readd_region(k_cache), READD_STRIPE_LEN, true);
	if (!legacy && unwritten)
		test_msg("raid56_wf_name_unwritten is set: unwritten members named, as expected");
	return readd_clear(rig, k_flushed, 4);
out:
	btrfs_wib_done(fs_info, readd_region(k_cache), READD_STRIPE_LEN, true);
	return ret;
mark_failed:
	test_err("mark failed: %d", ret);
	return ret;
}

/*
 * More regions than the wide layout describes, all of them with a name: the
 * records have to come back without the names, and the log has to go on
 * working.  Planned against the in-memory set alone, the readd named what fit
 * in the wide layout, owed the rest, and then could never write the union
 * with the last block again: every commit and every write failed until
 * unmount.
 */
static int readd_names_no_room(struct readd_rig *rig, bool legacy)
{
	struct btrfs_fs_info *fs_info = rig->fs_info;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 unnamed0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_LOG_UNFLUSHED]);
	const bool marks_off = btrfs_wib_all_records_torn();
	const u64 first = 10;
	const u64 nr = 100;
	void *saved;
	int ret;

	saved = kmalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!saved)
		return -ENOMEM;
	ret = readd_write_done(rig, first, nr);
	if (ret)
		goto out;
	memcpy(saved, wib->last, BTRFS_WIB_SLOT_SIZE);

	ret = readd_commit_failed(rig);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (btrfs_wib_block_drops(saved, wib->last)) {
		test_err("the commit whose flush failed dropped a region");
		goto out;
	}
	if (wib->readd_owed) {
		test_err("the records fit without the names, yet the readd is owed");
		goto out;
	}
	for (u64 k = first; k < first + nr; k++) {
		const struct btrfs_wib_entry *e = find_live_entry(wib, readd_region(k));

		if (!e || e->sticky != 0x3) {
			test_err("region %llu a failed flush kept is not recorded", k);
			goto out;
		}
	}
	if (btrfs_wib_any_stale(fs_info)) {
		test_err("names were applied to more regions than a wide block describes");
		goto out;
	}
	if (!legacy && atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_LOG_UNFLUSHED]) !=
	    unnamed0 + 1) {
		test_err("the device could not be named, and nothing said so");
		goto out;
	}
	/*
	 * What the names would have said, the records say as possibly torn:
	 * the blocks of the member each would have named.
	 */
	for (u64 k = first; k < first + nr; k++) {
		const struct btrfs_wib_entry *e = find_live_entry(wib, readd_region(k));
		u64 stale, stale_par, torn;

		readd_expect(k, &stale, &stale_par);
		torn = legacy || marks_off ? 0 : stale | stale_par;
		if (!e || e->torn != torn) {
			test_err("region %llu: marked possibly torn 0x%llx, expected 0x%llx",
				 k, e ? e->torn : 0, torn);
			goto out;
		}
		if (btrfs_wib_stripe_torn(fs_info, readd_region(k), READD_STRIPE_LEN) != !!torn) {
			test_err("region %llu: btrfs_wib_stripe_torn() does not see the mark", k);
			goto out;
		}
	}

	/* And the log goes on: a new write, a repair's persist, a commit. */
	ret = btrfs_wib_mark(fs_info, readd_region(first + nr), READD_STRIPE_LEN);
	if (ret) {
		test_err("a new write after the failed flush failed to mark: %d", ret);
		goto out;
	}
	btrfs_wib_done(fs_info, readd_region(first + nr), READD_STRIPE_LEN, false);
	ret = btrfs_wib_persist_now(fs_info);
	if (ret) {
		test_err("persist_now after the failed flush returned %d", ret);
		goto out;
	}
	ret = readd_commit(rig);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (btrfs_wib_block_drops(saved, wib->last)) {
		test_err("a region a failed flush kept was dropped by the next flushed commit");
		goto out;
	}
	/*
	 * The drop wrote the set as it is, nothing in flight: the marks are
	 * on disk as in flight, which is how a mount after a crash sees them.
	 */
	for (u64 k = first; k < first + nr; k++) {
		u64 stale, stale_par, torn;

		readd_expect(k, &stale, &stale_par);
		torn = legacy || marks_off || btrfs_wib_torn_no_persist() ? 0 :
		       stale | stale_par;
		if (block_bitmap(wib->last, readd_region(k)) != torn) {
			test_err("region %llu: the block lists 0x%llx in flight, expected the marks 0x%llx",
				 k, block_bitmap(wib->last, readd_region(k)), torn);
			goto out;
		}
	}
	ret = readd_clear(rig, first, nr + 1);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (btrfs_wib_stripe_torn(fs_info, readd_region(first), READD_STRIPE_LEN)) {
		test_err("a record the scrub retired is still marked possibly torn");
		goto out;
	}
	ret = 0;
out:
	kfree(saved);
	return ret;
}

/*
 * Writes in flight whose record is not written yet push the union past what
 * any block describes: the readd takes nothing back until they are done, then
 * everything, with the names.  A repair's persist must not wait for the next
 * transaction commit to find that out.
 */
static int readd_names_wait(struct readd_rig *rig, bool legacy)
{
	struct btrfs_fs_info *fs_info = rig->fs_info;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 first = 10;
	const u64 nr = 40;
	const u64 busy = 300;
	const u64 nr_busy = 130;
	unsigned long flags;
	void *saved;
	int ret = 0;

	saved = kmalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!saved)
		return -ENOMEM;
	ret = readd_write_done(rig, first, nr);
	if (ret)
		goto out;
	memcpy(saved, wib->last, BTRFS_WIB_SLOT_SIZE);
	spin_lock_irqsave(&wib->lock, flags);
	for (u64 k = busy; k < busy + nr_busy && !ret; k++)
		ret = btrfs_wib_try_mark(wib, readd_region(k), READD_STRIPE_LEN);
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret) {
		test_err("in-flight mark failed: %d", ret);
		goto out;
	}

	ret = readd_commit_failed(rig);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (btrfs_wib_block_drops(saved, wib->last)) {
		test_err("the commit whose flush failed dropped a region");
		goto out;
	}
	if (!legacy) {
		if (!wib->readd_owed) {
			test_err("no readd is owed although the union cannot be written");
			goto out;
		}
		ret = btrfs_wib_persist_now(fs_info);
		if (ret != -EIO) {
			test_err("persist_now returned %d while a readd was owed, expected -EIO",
				 ret);
			ret = -EINVAL;
			goto out;
		}
	}

	for (u64 k = busy; k < busy + nr_busy; k++)
		btrfs_wib_done(fs_info, readd_region(k), READD_STRIPE_LEN, false);
	ret = btrfs_wib_persist_now(fs_info);
	if (ret) {
		test_err("persist_now after the writes in flight finished returned %d", ret);
		goto out;
	}
	ret = -EINVAL;
	if (wib->readd_owed) {
		test_err("the readd is still owed with room to take everything back");
		goto out;
	}
	if (btrfs_wib_block_drops(saved, wib->last) && !legacy) {
		test_err("a region a failed flush kept was dropped");
		goto out;
	}
	for (u64 k = first; k < first + nr && !legacy; k++) {
		const struct btrfs_wib_entry *e = find_live_entry(wib, readd_region(k));
		u64 stale, stale_par;

		readd_expect(k, &stale, &stale_par);
		if (!e || e->sticky != 0x3 || e->stale != stale || e->stale_par != stale_par) {
			test_err("region %llu was not taken back with its name", k);
			goto out;
		}
	}
	ret = readd_clear(rig, first, nr);
out:
	kfree(saved);
	return ret;
}

/*
 * The set is wide already, and the last block lists more than a wide block
 * describes: no layout will ever hold both.  Take back what fits, lose the rest
 * loudly, and keep working -- owing a readd that can never complete would stop
 * every write for good.  What it took back names a member, so a write into a
 * region the log does not hold waits for a repair or a scrub to retire one
 * (see wib_evict_sticky()); a write into one it holds goes ahead.
 */
static int readd_names_lose(struct readd_rig *rig, bool legacy)
{
	struct btrfs_fs_info *fs_info = rig->fs_info;
	struct btrfs_wib *wib = fs_info->wib;
	const u64 dropped0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]);
	const u64 first = 10;
	const u64 nr = 100;
	const u64 k_stale = 150;
	int ret;

	ret = readd_write_done(rig, first, nr);
	if (ret)
		return ret;
	btrfs_wib_add_sticky(fs_info, readd_region(k_stale), READD_STRIPE_LEN);
	btrfs_wib_mark_stale(fs_info, readd_region(k_stale), BTRFS_WIB_BLOCK_SIZE);

	ret = readd_commit_failed(rig);
	if (ret)
		return ret;
	if (wib->readd_owed) {
		test_err("owed a readd no layout can ever hold");
		return -EINVAL;
	}
	if (legacy) {
		/*
		 * The negative control: the old readd takes back only what the
		 * table has room for, and says so only outside a self test
		 * (wib_readd_legacy() returns before the alert when testing,
		 * as the readd it restores did).  What has to show is the loss.
		 */
		u32 kept = 0;

		for (u64 k = first; k < first + nr; k++) {
			const struct btrfs_wib_entry *e = find_live_entry(wib, readd_region(k));

			if (e && (e->sticky & 0x3) == 0x3)
				kept++;
		}
		if (kept == nr) {
			test_err("raid56_wf_no_readd_name is set, yet the readd kept every region: the test is blind");
			return -EINVAL;
		}
		test_msg("raid56_wf_no_readd_name is set: %u of %llu regions lost, as expected",
			 (u32)(nr - kept), nr);
	} else if (atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_DROPPED]) == dropped0) {
		test_err("records were lost, and nothing said so");
		return -EINVAL;
	}
	if (!legacy) {
		u32 named = 0;

		for (u64 k = first; k < first + nr; k++) {
			const struct btrfs_wib_entry *e = find_live_entry(wib, readd_region(k));

			if (e && (e->stale | e->stale_par))
				named++;
		}
		if (!named) {
			test_err("the set is wide anyway, yet nothing taken back was named");
			return -EINVAL;
		}
	}
	/* The log goes on. */
	ret = readd_commit(rig);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) > BTRFS_WIB_MAX_ENTRIES) {
		test_err("a wide block lists %u regions", block_nr_entries(wib->last));
		return -EINVAL;
	}
	ret = btrfs_wib_mark(fs_info, readd_region(k_stale), READD_STRIPE_LEN);
	if (ret) {
		test_err("a new write after the loss failed to mark: %d", ret);
		return ret;
	}
	btrfs_wib_done(fs_info, readd_region(k_stale), READD_STRIPE_LEN, false);
	return readd_clear(rig, first, k_stale + 1 - first);
}

/* Is a repair of the full stripe at @logical waiting? */
static bool repair_queued(struct btrfs_wib *wib, u64 logical)
{
	bool ret = false;

	spin_lock(&wib->repair_lock);
	for (unsigned int i = 0; i < wib->repair_nr; i++)
		if (wib->repair_queue[i].logical == logical)
			ret = true;
	spin_unlock(&wib->repair_lock);
	return ret;
}

/*
 * A failed flush that names a member asks for its full stripe to be repaired,
 * and says so, as a write the device failed would: while every device is
 * there the record is not spent, and nothing else retires it until a write
 * reaches the stripe or a scrub runs -- a log full of them fails every write
 * into a new region.  Not a stripe it names nothing in.  With
 * raid56_wf_readd_no_repair=1 nothing is asked for, and nothing said.
 */
static int readd_names_repair(struct readd_rig *rig, bool legacy)
{
	struct btrfs_fs_info *fs_info = rig->fs_info;
	struct btrfs_wib *wib = fs_info->wib;
	const bool asks = !legacy && !btrfs_wib_readd_no_repair() &&
			  !btrfs_raid56_no_repair_on_fault();
	const u64 stale0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_STALE]);
	const u64 first = 470;
	const u64 nr = 6;
	/* Nothing went to its member on the failed device. */
	const u64 k_unwritten = first + nr;
	u64 stale, stale_par;
	int ret;

	clear_repairs(wib);
	ret = readd_write_done(rig, first, nr);
	if (ret)
		return ret;
	ret = btrfs_wib_mark(fs_info, readd_region(k_unwritten), READD_STRIPE_LEN);
	if (ret) {
		test_err("mark of region %llu failed: %d", k_unwritten, ret);
		return ret;
	}
	readd_expect(k_unwritten, &stale, &stale_par);
	readd_issue(rig, k_unwritten, 0x3 & ~stale, stale_par ? 0 : 0x1);
	btrfs_wib_done(fs_info, readd_region(k_unwritten), READD_STRIPE_LEN, false);
	ret = readd_commit_failed(rig);
	if (ret)
		return ret;
	for (u64 k = first; k <= k_unwritten; k++) {
		bool want = asks;

		readd_expect(k, &stale, &stale_par);
		if (k == k_unwritten)
			want = asks && btrfs_wib_name_unwritten();
		if (!(stale | stale_par)) {
			test_err("the rig puts nothing of region %llu on the failed device", k);
			return -EINVAL;
		}
		if (repair_queued(wib, readd_region(k)) != want) {
			test_err("region %llu: a repair %s asked for, names 0x%llx 0x%llx",
				 k, want ? "was not" : "was", stale, stale_par);
			return -EINVAL;
		}
	}
	if ((atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_STALE]) != stale0) ==
	    (legacy || btrfs_wib_readd_no_repair())) {
		test_err("the stripes a failed flush named %s announced",
			 legacy || btrfs_wib_readd_no_repair() ? "were" : "were not");
		return -EINVAL;
	}
	if (!asks)
		test_msg("repairs of the stripes a failed flush named are off: none asked for, as expected");
	clear_repairs(wib);
	return readd_clear(rig, first, nr + 1);
}

/* Does @block list anything of region @bytenr? */
static bool block_lists(const void *block, u64 bytenr)
{
	for (u32 i = 0; i < block_nr_entries(block); i++) {
		struct btrfs_wib_entry e;

		btrfs_wib_read_entry(block, i, &e);
		if (e.bytenr == bytenr && (e.bitmap | e.sticky))
			return true;
	}
	return false;
}

/*
 * A disable writes the log's last block like any drop, and a failed flush
 * there names what a write went to as it does with the log enabled.  Here the
 * disabling commit's own barrier fails on the device of a member written
 * since its last good flush: the stripe stays recorded, that member named.
 * The disable used to forget what was written before that block, and not to
 * look at the barrier at all (raid56_wf_disable_forgets_writes=1): the flush
 * that wrote the block then dropped the stripe.
 */
static int readd_names_disable(struct readd_rig *rig, bool legacy)
{
	struct btrfs_fs_info *fs_info = rig->fs_info;
	struct btrfs_wib *wib = fs_info->wib;
	const bool forgets = btrfs_wib_disable_forgets_writes();
	const u64 k = 510;
	const struct btrfs_wib_entry *e;
	u64 stale, stale_par;
	bool rows = false;
	int ret;

	readd_expect(k, &stale, &stale_par);
	if (!(stale | stale_par)) {
		test_err("the rig puts nothing of region %llu on the failed device", k);
		return -EINVAL;
	}

	/* A commit writes a superblock without the flag: the disable is armed. */
	btrfs_set_super_compat_ro_flags(fs_info->super_for_commit, 0);
	btrfs_wib_disable(fs_info);
	ret = readd_commit(rig);
	if (ret)
		return ret;
	if (!wib->enabled) {
		test_err("disabled before the superblock without the flag was written");
		return -EINVAL;
	}
	/* A write into region k, recorded and finished. */
	ret = btrfs_wib_mark(fs_info, readd_region(k), READD_STRIPE_LEN);
	if (ret) {
		test_err("mark of region %llu failed: %d", k, ret);
		return ret;
	}
	readd_issue(rig, k, 0x3, 0x1);
	btrfs_wib_done(fs_info, readd_region(k), READD_STRIPE_LEN, false);
	/* The disabling commit, its barrier failing on that device. */
	ret = readd_commit_failed(rig);
	if (ret)
		return ret;
	if (wib->enabled) {
		test_err("the log was not disabled");
		return -EINVAL;
	}

	ret = -EINVAL;
	e = find_live_entry(wib, readd_region(k));
	if (forgets) {
		if (e || block_lists(wib->last, readd_region(k))) {
			test_err("raid56_wf_disable_forgets_writes is set, yet the disable kept the stripe: the test is blind");
			goto out;
		}
		test_msg("raid56_wf_disable_forgets_writes is set: stripe dropped, as expected");
	} else {
		if (!e || !(e->sticky & 0x3) || !block_lists(wib->last, readd_region(k))) {
			test_err("the disable's last block dropped a stripe its failed barrier covered");
			goto out;
		}
		if (legacy ? (e->stale | e->stale_par) :
			     (e->stale != stale || e->stale_par != stale_par)) {
			test_err("region %llu: stale 0x%llx stale_par 0x%llx after the disable, expected 0x%llx 0x%llx",
				 k, e->stale, e->stale_par, legacy ? 0 : stale,
				 legacy ? 0 : stale_par);
			goto out;
		}
	}
	/* Nothing owed: the written records went with the log. */
	for (int i = 0; i < BTRFS_WIB_WRITTEN_SLOTS; i++)
		if (wib->written[i].cols | wib->written[i].par)
			rows = true;
	if (wib->readd_owed || wib->written_unknown || rows) {
		test_err("written records outlived the disable: owed %d unknown %d rows %d",
			 wib->readd_owed, wib->written_unknown, rows);
		goto out;
	}
	ret = 0;
out:
	/* Enabled again for the rest. */
	btrfs_wib_request_enable(fs_info, true);
	if (readd_commit(rig) || !wib->enabled) {
		test_err("could not enable the log again");
		return -EINVAL;
	}
	if (ret)
		return ret;
	return readd_clear(rig, k, 1);
}

static int test_readd_names(struct btrfs_fs_info *fs_info)
{
	const bool legacy = btrfs_wib_readd_legacy();
	struct readd_rig rig = { .fs_info = fs_info };
	int ret;

	rig.map = btrfs_alloc_chunk_map(READD_NR_DEVS, GFP_KERNEL);
	if (!rig.map) {
		test_std_err(TEST_ALLOC_CHUNK_MAP);
		return -ENOMEM;
	}
	rig.map->start = READD_CHUNK;
	rig.map->stripe_size = SZ_1G;
	rig.map->chunk_len = READD_NR_DATA * rig.map->stripe_size;
	rig.map->num_stripes = READD_NR_DEVS;
	rig.map->type = BTRFS_BLOCK_GROUP_DATA | BTRFS_BLOCK_GROUP_RAID5;
	for (int i = 0; i < READD_NR_DEVS; i++) {
		struct btrfs_device *dev = btrfs_alloc_dummy_device(fs_info);

		if (IS_ERR(dev)) {
			test_err("cannot allocate device");
			btrfs_free_chunk_map(rig.map);
			return PTR_ERR(dev);
		}
		/* Not WRITEABLE: the log writes nothing to them. */
		dev->devid = 101 + i;
		rig.devs[i] = dev;
		rig.map->stripes[i].dev = dev;
		rig.map->stripes[i].physical = SZ_1M;
	}
	ret = btrfs_add_chunk_map(fs_info, rig.map);
	if (ret) {
		test_err("error adding chunk map to mapping tree");
		btrfs_free_chunk_map(rig.map);
		return ret;
	}

	ret = readd_names_which(&rig, legacy);
	if (!ret)
		ret = readd_names_spent_torn(&rig, legacy);
	if (!ret)
		ret = readd_names_written(&rig, legacy);
	if (!ret)
		ret = readd_names_no_room(&rig, legacy);
	if (!ret)
		ret = readd_names_wait(&rig, legacy);
	if (!ret)
		ret = readd_names_lose(&rig, legacy);
	if (!ret)
		ret = readd_names_disable(&rig, legacy);
	if (!ret)
		ret = readd_names_repair(&rig, legacy);
	clear_repairs(fs_info->wib);
	if (!ret && legacy)
		test_msg("raid56_wf_no_readd_name is set: nothing was named, as expected");
	btrfs_remove_chunk_map(fs_info, rig.map);
	return ret;
}

/*
 * A record whose chunk has been removed must go with it.
 *
 * The bits are interpreted against whatever chunk covers their address when
 * they are read.  Once the address is reallocated they are read with a
 * different geometry, so a surviving record makes a scrub believe a column of
 * the NEW chunk is stale and rebuild it from a parity that was describing it
 * correctly -- the record meant to prevent a misrepair causes one.
 */
/*
 * A stripe whose only record is a parity that does not describe the data must
 * still be visible to the staleness queries.  They all start with a lock-free
 * "nothing is recorded stale" check, which used to count only stale data: a
 * parity-only record left it at zero, so a degraded read rebuilt from that
 * parity without knowing it was wrong.
 */
static int test_stale_parity_only(struct btrfs_fs_info *fs_info)
{
	const u64 stripe = 940ULL * BTRFS_WIB_ENTRY_SIZE;
	struct btrfs_wib_stripe_state st;

	if (btrfs_wib_any_stale(fs_info)) {
		test_err("something was already recorded stale before the parity test");
		return -EINVAL;
	}
	btrfs_wib_add_sticky(fs_info, stripe, 3 * BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_update_stale_parity(fs_info, stripe, 0, true);
	if (!btrfs_wib_any_stale(fs_info)) {
		test_err("a parity-only record is invisible to the lock-free check");
		return -EINVAL;
	}
	if (!btrfs_wib_stripe_state(fs_info, stripe, 3, 1, &st) || st.bad_parity != 1) {
		test_err("a parity-only record did not reach the stripe state");
		return -EINVAL;
	}
	btrfs_wib_update_stale_parity(fs_info, stripe, 0, false);
	if (btrfs_wib_any_stale(fs_info)) {
		test_err("clearing the parity record left the stale count up");
		return -EINVAL;
	}
	btrfs_wib_update_stale_parity(fs_info, stripe, 0, true);
	btrfs_wib_clear_sticky(fs_info, stripe, 3 * BTRFS_WIB_BLOCK_SIZE);
	if (btrfs_wib_any_stale(fs_info)) {
		test_err("retiring the record left the parity counted stale");
		return -EINVAL;
	}
	return 0;
}

static int test_forget_range(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 chunk = 900ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 len = 2 * BTRFS_WIB_ENTRY_SIZE;
	const u64 keep = 910ULL * BTRFS_WIB_ENTRY_SIZE;
	int ret;

	btrfs_wib_add_sticky(fs_info, chunk, len);
	btrfs_wib_mark_stale(fs_info, chunk, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_update_stale_parity(fs_info, chunk, 0, true);
	btrfs_wib_add_sticky(fs_info, keep, BTRFS_WIB_BLOCK_SIZE);

	if (!find_live_entry(wib, chunk) || !btrfs_wib_any_stale(fs_info)) {
		test_err("the chunk's records were not created");
		return -EINVAL;
	}

	btrfs_wib_forget_range(fs_info, chunk, len);

	if (find_live_entry(wib, chunk) ||
	    find_live_entry(wib, chunk + BTRFS_WIB_ENTRY_SIZE)) {
		test_err("a record survived the removal of its chunk");
		return -EINVAL;
	}
	if (btrfs_wib_any_stale(fs_info)) {
		test_err("nr_stale was not decremented when the chunk was forgotten");
		return -EINVAL;
	}
	if (!find_live_entry(wib, keep)) {
		test_err("forgetting a chunk took a record outside it");
		return -EINVAL;
	}

	btrfs_wib_clear_sticky(fs_info, keep, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the forget-range test");
		return -EINVAL;
	}
	return 0;
}

/*
 * A device replace's own stale marks (btrfs_wib_replace_mark_stale()): left
 * out of the queries while the replace runs, since the source still serves the
 * column; kept through a retirement that did not look at them; handed over
 * when the replace finishes and dropped when it does not -- but never dropped
 * once something else has marked the same member, which then says the source
 * is stale there too.
 */
static int test_replace_marks(struct btrfs_fs_info *fs_info)
{
	const u64 stripe = 960ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 len = 2 * BTRFS_WIB_BLOCK_SIZE;
	const u64 col1 = stripe + BTRFS_WIB_BLOCK_SIZE;
	struct btrfs_wib_stripe_state st;

	if (btrfs_wib_any_stale(fs_info)) {
		test_err("something was already recorded stale before the replace test");
		return -EINVAL;
	}

	btrfs_wib_add_sticky(fs_info, stripe, len);
	if (!btrfs_wib_replace_mark_stale(fs_info, col1, true)) {
		test_err("the replace's mark was not kept");
		return -EINVAL;
	}
	if (!btrfs_wib_any_stale(fs_info)) {
		test_err("the replace's mark is not counted");
		return -EINVAL;
	}
	if (btrfs_wib_stale(fs_info, col1) ||
	    btrfs_wib_stripe_state(fs_info, stripe, 2, 1, &st)) {
		test_err("a running replace's own mark is visible to the source's readers");
		return -EINVAL;
	}
	btrfs_wib_clear_sticky(fs_info, stripe, len);
	btrfs_wib_replace_end(fs_info, true);
	if (!btrfs_wib_stale(fs_info, col1)) {
		test_err("the record was retired under a running replace, or the finished replace did not hand its mark over");
		return -EINVAL;
	}
	btrfs_wib_clear_sticky(fs_info, stripe, len);
	if (btrfs_wib_any_stale(fs_info)) {
		test_err("the handed-over record could not be retired");
		return -EINVAL;
	}

	btrfs_wib_add_sticky(fs_info, stripe, len);
	if (!btrfs_wib_replace_mark_stale(fs_info, col1, true) ||
	    !btrfs_wib_replace_mark_parity(fs_info, stripe, 0, true)) {
		test_err("the replace's marks were not kept");
		return -EINVAL;
	}
	btrfs_wib_replace_end(fs_info, false);
	if (btrfs_wib_any_stale(fs_info)) {
		test_err("a replace that did not finish left its marks");
		return -EINVAL;
	}
	if (btrfs_wib_stripe_error(fs_info, stripe, 2) != BTRFS_WIB_STRIPE_ERROR) {
		test_err("a replace that did not finish took the stripe's record with its marks");
		return -EINVAL;
	}

	btrfs_wib_replace_mark_stale(fs_info, col1, true);
	btrfs_wib_mark_stale(fs_info, col1, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_replace_mark_parity(fs_info, stripe, 0, true);
	btrfs_wib_update_stale_parity(fs_info, stripe, 0, true);
	if (!btrfs_wib_stale(fs_info, col1) ||
	    !btrfs_wib_stripe_state(fs_info, stripe, 2, 1, &st) ||
	    st.stale_cols != 2 || st.bad_parity != 1) {
		test_err("marks something else made too stayed hidden");
		return -EINVAL;
	}
	btrfs_wib_replace_end(fs_info, false);
	if (!btrfs_wib_stale(fs_info, col1) ||
	    !btrfs_wib_stripe_state(fs_info, stripe, 2, 1, &st) ||
	    st.stale_cols != 2 || st.bad_parity != 1) {
		test_err("a replace that did not finish dropped marks that were not only its own");
		return -EINVAL;
	}

	/* Stale before the replace marked it: not the replace's to drop. */
	btrfs_wib_replace_mark_stale(fs_info, col1, true);
	btrfs_wib_replace_end(fs_info, false);
	if (!btrfs_wib_stale(fs_info, col1)) {
		test_err("a replace that did not finish dropped a mark from before it");
		return -EINVAL;
	}

	btrfs_wib_clear_sticky(fs_info, stripe, len);
	if (btrfs_wib_any_stale(fs_info)) {
		test_err("retiring the record left marks counted stale");
		return -EINVAL;
	}
	return 0;
}

/*
 * A log that is legal in the narrow layout must not become unwritable when a
 * stale bit forces the wide one.
 *
 * The in-memory table holds BTRFS_WIB_MAX_ENTRIES_V1 regions, and while
 * nothing is stale a block describes exactly that many.  The first stale bit
 * halves what a block can describe, without anything having been added -- and
 * a set that cannot be described cannot be written.  btrfs_wib_build_block()
 * then returns -ENOSPC leaving the block zeroed with no magic, and
 * wib_flush_and_drop_locked() hands that on as the snapshot of what was in
 * flight: an empty snapshot reads as "nothing was in flight", so stripes that
 * finished after the flush are dropped from the log with no flush covering
 * them.  That is the write hole, reopened by an accounting mistake.
 *
 * So the live set is capped at what the current layout can express, and going
 * stale spends records to get back under it.
 */
static int test_capacity_follows_layout(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 base = 2000;
	const u32 wide = BTRFS_WIB_MAX_ENTRIES;
	const u32 narrow = BTRFS_WIB_MAX_ENTRIES_V1;
	const u64 named = (base + narrow - 1) * BTRFS_WIB_ENTRY_SIZE;
	void *block;
	u32 live = 0;
	int ret;

	block = kzalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!block)
		return -ENOMEM;

	/* Fill past the wide maximum with records nothing has named. */
	for (u32 i = 0; i < narrow; i++)
		btrfs_wib_add_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				     BTRFS_WIB_BLOCK_SIZE);
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++)
		if (wib->entries[i].bitmap | wib->entries[i].sticky)
			live++;
	if (live <= wide) {
		test_err("expected more than %u live regions, got %u", wide, live);
		ret = -EINVAL;
		goto out;
	}

	/* Legal in the narrow layout. */
	ret = btrfs_wib_build_block(wib, block, 1, NULL);
	if (ret) {
		test_err("a narrow-layout set of %u regions did not build: %d",
			 live, ret);
		goto out;
	}

	/* One stale bit halves what a block can say. */
	btrfs_wib_mark_stale(fs_info, named, BTRFS_WIB_BLOCK_SIZE);
	if (!btrfs_wib_any_stale(fs_info)) {
		test_err("the record did not go stale");
		ret = -EINVAL;
		goto out;
	}

	ret = btrfs_wib_build_block(wib, block, 2, NULL);
	if (ret) {
		test_err("the log became unwritable when a record went stale: %d",
			 ret);
		goto out;
	}
	if (!(le64_to_cpu(((struct btrfs_wib_disk_header *)block)->flags) &
	      BTRFS_WIB_FLAG_STALE)) {
		test_err("a block carrying a stale record was not written wide");
		ret = -EINVAL;
		goto out;
	}
	if (block_nr_entries(block) > wide) {
		test_err("wide block claims %u entries, more than the %u it can hold",
			 block_nr_entries(block), wide);
		ret = -EINVAL;
		goto out;
	}
	/* The record that names a member is the one that had to survive. */
	if (!find_live_entry(wib, wib_test_entry_base(named))) {
		test_err("the named record was spent to make the set fit");
		ret = -EINVAL;
		goto out;
	}

	for (u32 i = 0; i < narrow; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		goto out;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the capacity test");
		ret = -EINVAL;
		goto out;
	}
	ret = 0;
out:
	kfree(block);
	return ret;
}

/*
 * The mount's verdict on a possibly torn stripe it cannot decide
 * (btrfs_wib_mark_suspect_parity()) is a stale parity in memory and the
 * stripe's torn mark on disk: it does not halve the log.  More such stripes
 * than a wide block holds all stay listed, in a narrow block, as in flight.
 * A block something else makes wide carries the verdict too, and a parity mark
 * any other writer sets is its own.  Under raid56_wf_suspect_as_stale=1, or a
 * knob that leaves no torn mark to fall back on, the verdict counts as stale
 * and a log with no device missing keeps what a wide block holds.
 */
static int test_suspect_narrow(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool widens = btrfs_wib_suspect_as_stale() ||
			    btrfs_wib_torn_no_persist() ||
			    btrfs_wib_all_records_torn();
	const u32 nr = BTRFS_WIB_MAX_ENTRIES + 4;
	const u64 base = 3200;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	const u64 len = 3 * blk;
	/* At block 8 of each region: parity 0's mark is bit 8. */
	const u64 off = 8 * blk;
	const u64 first = base * BTRFS_WIB_ENTRY_SIZE;
	const u64 other = (base + nr) * BTRFS_WIB_ENTRY_SIZE;
	struct btrfs_wib_stripe_state st;
	struct btrfs_wib_entry e = { 0 };
	void *block;
	u32 live = 0;
	int ret;

	block = kzalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!block)
		return -ENOMEM;

	for (u32 i = 0; i < nr; i++) {
		const u64 start = (base + i) * BTRFS_WIB_ENTRY_SIZE + off;

		btrfs_wib_add_sticky(fs_info, start, len);
		btrfs_wib_mark_suspect_parity(fs_info, start, len, 0);
	}
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++)
		if (wib->entries[i].bitmap | wib->entries[i].sticky)
			live++;
	ret = -EINVAL;
	if (widens) {
		if (live > BTRFS_WIB_MAX_ENTRIES) {
			test_err("raid56_wf_suspect_as_stale or a knob without the torn mark is set, yet %u undecided stripes stayed listed",
				 live);
			goto out;
		}
		test_msg("undecided stripes count as stale: %u of %u listed, as expected",
			 live, nr);
		ret = 0;
		goto out;
	}
	if (live != nr) {
		test_err("%u of %u undecided stripes stayed listed", live, nr);
		goto out;
	}
	if (!btrfs_wib_stripe_state(fs_info, first + off, 3, 1, &st) ||
	    st.bad_parity != 1) {
		test_err("the verdict is not a stale parity in memory");
		goto out;
	}
	ret = btrfs_wib_build_block(wib, block, 1, NULL);
	if (ret) {
		test_err("%u undecided stripes did not fit a block: %d", nr, ret);
		goto out;
	}
	ret = -EINVAL;
	if ((le64_to_cpu(((struct btrfs_wib_disk_header *)block)->flags) &
	     BTRFS_WIB_FLAG_STALE) || block_nr_entries(block) != nr ||
	    block_bitmap(block, first) != 0x700) {
		test_err("block: flags 0x%llx, %u regions, in flight 0x%llx; expected narrow, %u, 0x700",
			 le64_to_cpu(((struct btrfs_wib_disk_header *)block)->flags),
			 block_nr_entries(block), block_bitmap(block, first), nr);
		goto out;
	}
	if (!btrfs_wib_can_mark(wib, other, blk)) {
		test_err("the verdicts left no room in a log holding %u regions", nr);
		goto out;
	}

	/* Down to one, and a record elsewhere names a column: wide, with the verdict. */
	for (u32 i = 1; i < nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE + off,
				       len);
	btrfs_wib_add_sticky(fs_info, other, blk);
	btrfs_wib_mark_stale(fs_info, other, blk);
	ret = btrfs_wib_build_block(wib, block, 2, NULL);
	if (ret) {
		test_err("building the wide block failed: %d", ret);
		goto out;
	}
	ret = -EINVAL;
	for (u32 i = 0; i < block_nr_entries(block); i++) {
		btrfs_wib_read_entry(block, i, &e);
		if (e.bytenr == first)
			break;
		e.stale_par = 0;
	}
	if (!(le64_to_cpu(((struct btrfs_wib_disk_header *)block)->flags) &
	      BTRFS_WIB_FLAG_STALE) || e.stale_par != 0x100) {
		test_err("a wide block did not carry the verdict: stale_par 0x%llx",
			 e.stale_par);
		goto out;
	}

	/* The other record gone: narrow again, until a write owns the mark. */
	btrfs_wib_clear_sticky(fs_info, other, blk);
	ret = btrfs_wib_build_block(wib, block, 3, NULL);
	if (ret || (le64_to_cpu(((struct btrfs_wib_disk_header *)block)->flags) &
		    BTRFS_WIB_FLAG_STALE)) {
		test_err("the verdict alone made the block wide: %d", ret);
		ret = -EINVAL;
		goto out;
	}
	btrfs_wib_update_stale_parity(fs_info, first + off, 0, true);
	ret = btrfs_wib_build_block(wib, block, 4, NULL);
	if (ret || !(le64_to_cpu(((struct btrfs_wib_disk_header *)block)->flags) &
		     BTRFS_WIB_FLAG_STALE)) {
		test_err("a parity mark a write took over was left out of the block: %d",
			 ret);
		ret = -EINVAL;
		goto out;
	}
	ret = 0;
out:
	for (u32 i = 0; i < nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE + off,
				       len);
	btrfs_wib_clear_sticky(fs_info, other, blk);
	if (!ret && btrfs_wib_any_stale(fs_info)) {
		test_err("the undecided stripes left something counted stale");
		ret = -EINVAL;
	}
	kfree(block);
	return ret;
}

/*
 * A full log with a device missing spends records that name a member rather
 * than fail the write (wib_may_evict_naming()), and the recovery's verdicts
 * last: dropping one turns a read that fails into a rebuild from a torn
 * parity.  Under raid56_wf_suspect_as_stale=1 they go in table order, and
 * the verdict in the first slot is the first spent.
 */
static int test_suspect_spent_last(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool as_stale = btrfs_wib_suspect_as_stale();
	const u64 evicted0 = atomic64_read(&wib->stat_sticky_evicted);
	const u64 base = 3400;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	const u64 verdict = base * BTRFS_WIB_ENTRY_SIZE;
	const u32 nr = BTRFS_WIB_MAX_ENTRIES;
	bool kept;
	int ret = -EINVAL;

	if (btrfs_wib_keeps_naming_degraded()) {
		test_msg("raid56_keep_naming_degraded is set, skipping the spent-last test");
		return 0;
	}
	if (wib->entries[0].bitmap | wib->entries[0].sticky) {
		test_err("the spent-last test needs the first slot free");
		return -EINVAL;
	}
	btrfs_wib_add_sticky(fs_info, verdict, 3 * blk);
	btrfs_wib_mark_suspect_parity(fs_info, verdict, 3 * blk, 0);
	/* Records naming a column fill the rest of what a wide block holds. */
	for (u32 i = 1; i < nr; i++) {
		const u64 logical = (base + i) * BTRFS_WIB_ENTRY_SIZE;

		btrfs_wib_add_sticky(fs_info, logical, blk);
		btrfs_wib_mark_stale(fs_info, logical, blk);
	}
	if (atomic64_read(&wib->stat_sticky_evicted) != evicted0) {
		test_err("a record was spent before the log was full");
		goto out;
	}
	/* Degraded: one more such record makes the log spend one. */
	fs_info->fs_devices->missing_devices = 1;
	btrfs_wib_add_sticky(fs_info, (base + nr) * BTRFS_WIB_ENTRY_SIZE, blk);
	btrfs_wib_mark_stale(fs_info, (base + nr) * BTRFS_WIB_ENTRY_SIZE, blk);
	fs_info->fs_devices->missing_devices = 0;
	kept = find_live_entry(wib, verdict);
	if (atomic64_read(&wib->stat_sticky_evicted) != evicted0 + 1 ||
	    !find_live_entry(wib, (base + nr) * BTRFS_WIB_ENTRY_SIZE)) {
		test_err("a full degraded log spent %llu records to take one",
			 atomic64_read(&wib->stat_sticky_evicted) - evicted0);
		goto out;
	}
	if (kept == as_stale) {
		if (as_stale)
			test_err("raid56_wf_suspect_as_stale is set, yet the verdict in the first slot was not spent first: the test is blind");
		else
			test_err("a full degraded log spent the recovery's verdict before a record naming a column");
		goto out;
	}
	if (as_stale)
		test_msg("raid56_wf_suspect_as_stale is set: verdict spent first, as expected");
	ret = 0;
out:
	fs_info->fs_devices->missing_devices = 0;
	for (u32 i = 0; i <= nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       i ? blk : 3 * blk);
	if (!ret && btrfs_wib_any_stale(fs_info)) {
		test_err("the spent-last test left something counted stale");
		ret = -EINVAL;
	}
	return ret;
}

/*
 * A device replace that ends, finished or not, releases its own marks and
 * nothing else: the recovery's verdicts are not its own (@suspect_par).
 * Released with them, they became ordinary stale parities -- every block wide
 * at once, and a log holding more of them than a wide block describes could no
 * longer be written, with nothing to make it fit.  Under
 * raid56_wf_replace_end_clears_verdicts=1 the block does not build.
 */
static int test_suspect_replace_end(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool clears = btrfs_wib_replace_end_clears_verdicts();
	const u32 nr = BTRFS_WIB_MAX_ENTRIES + 4;
	const u64 base = 3600;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	const u64 len = 3 * blk;
	/* At block 8 of each region, as in test_suspect_narrow(). */
	const u64 off = 8 * blk;
	u32 live = 0;
	u32 verdicts = 0;
	void *block;
	int ret;

	if (btrfs_wib_suspect_as_stale() || btrfs_wib_torn_no_persist() ||
	    btrfs_wib_all_records_torn()) {
		test_msg("the verdict counts as stale under the knobs set, skipping the replace-end test");
		return 0;
	}
	block = kzalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!block)
		return -ENOMEM;

	for (u32 i = 0; i < nr; i++) {
		const u64 start = (base + i) * BTRFS_WIB_ENTRY_SIZE + off;

		btrfs_wib_add_sticky(fs_info, start, len);
		btrfs_wib_mark_suspect_parity(fs_info, start, len, 0);
	}
	btrfs_wib_replace_end(fs_info, true);
	btrfs_wib_replace_end(fs_info, false);
	for (int i = 0; i < BTRFS_WIB_NR_ENTRIES; i++) {
		if (wib->entries[i].bitmap | wib->entries[i].sticky)
			live++;
		if (wib->entries[i].suspect_par)
			verdicts++;
	}
	ret = btrfs_wib_build_block(wib, block, 1, NULL);
	if (clears) {
		if (ret != -ENOSPC || verdicts) {
			test_err("raid56_wf_replace_end_clears_verdicts is set, yet %u verdicts are left and the block built with %d: the test is blind",
				 verdicts, ret);
			ret = -EINVAL;
			goto out;
		}
		test_msg("raid56_wf_replace_end_clears_verdicts is set: no verdict left, %u regions do not fit a block, as expected",
			 live);
		ret = 0;
		goto out;
	}
	if (ret) {
		test_err("the end of a replace left %u regions the log cannot write: %d",
			 live, ret);
		goto out;
	}
	ret = -EINVAL;
	if (live != nr || verdicts != nr) {
		test_err("the end of a replace left %u regions, %u of them verdicts, of %u",
			 live, verdicts, nr);
		goto out;
	}
	if (le64_to_cpu(((struct btrfs_wib_disk_header *)block)->flags) &
	    BTRFS_WIB_FLAG_STALE) {
		test_err("the end of a replace made the verdicts' block wide");
		goto out;
	}
	ret = 0;
out:
	for (u32 i = 0; i < nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE + off,
				       len);
	if (!ret && btrfs_wib_any_stale(fs_info)) {
		test_err("the replace-end test left something counted stale");
		ret = -EINVAL;
	}
	kfree(block);
	return ret;
}

/*
 * A full log with a device missing spends a running device replace's own marks
 * -- its record of the zeros it put on its target -- after every other record
 * naming a member, and a replace that lost one anyway fails
 * (btrfs_wib_replace_end() -EIO, with a latched alert) rather than let its
 * target serve the zeros as data.  Under raid56_wf_evict_replace_marks=1 the
 * mark in the first slot is spent first, and the replace finishes.
 */
static int test_replace_marks_spent_last(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool evicts = btrfs_wib_evicts_replace_marks();
	const u64 alerts0 = atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_REPLACE_DROPPED]);
	const u64 base = 3800;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	const u64 owned = base * BTRFS_WIB_ENTRY_SIZE;
	const u64 extra = (base + BTRFS_WIB_MAX_ENTRIES) * BTRFS_WIB_ENTRY_SIZE;
	const u32 nr = BTRFS_WIB_MAX_ENTRIES;
	bool lost;
	int ret = -EINVAL;
	int end;

	if (btrfs_wib_keeps_naming_degraded()) {
		test_msg("raid56_keep_naming_degraded is set, skipping the replace-marks test");
		return 0;
	}
	if (wib->entries[0].bitmap | wib->entries[0].sticky) {
		test_err("the replace-marks test needs the first slot free");
		return -EINVAL;
	}

	/* The replace's record of zeros on its target, in the first slot. */
	btrfs_wib_add_sticky(fs_info, owned, 2 * blk);
	if (!btrfs_wib_replace_mark_stale(fs_info, owned + blk, true)) {
		test_err("the replace's mark was not kept");
		goto out;
	}
	/* Records naming a column fill the rest of what a wide block holds. */
	for (u32 i = 1; i < nr; i++) {
		const u64 logical = (base + i) * BTRFS_WIB_ENTRY_SIZE;

		btrfs_wib_add_sticky(fs_info, logical, blk);
		btrfs_wib_mark_stale(fs_info, logical, blk);
	}
	/* Degraded: one more such record makes the log spend one. */
	fs_info->fs_devices->missing_devices = 1;
	btrfs_wib_add_sticky(fs_info, extra, blk);
	btrfs_wib_mark_stale(fs_info, extra, blk);
	fs_info->fs_devices->missing_devices = 0;
	if (!find_live_entry(wib, extra)) {
		test_err("a full degraded log did not take the new record");
		goto out;
	}
	if (!find_live_entry(wib, owned) != evicts) {
		if (evicts)
			test_err("raid56_wf_evict_replace_marks is set, yet the replace's mark in the first slot was not spent first: the test is blind");
		else
			test_err("a full degraded log spent a replace's record of zeros on its target before a record naming a column");
		goto out;
	}
	if (btrfs_wib_replace_marks_lost(fs_info)) {
		test_err("the replace was failed although no mark of its was spent");
		goto out;
	}

	/* Now nothing but the replace's marks to spend. */
	btrfs_wib_replace_end(fs_info, false);
	for (u32 i = 0; i <= nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       i ? blk : 2 * blk);
	for (u32 i = 0; i < nr; i++) {
		const u64 logical = (base + i) * BTRFS_WIB_ENTRY_SIZE;

		btrfs_wib_add_sticky(fs_info, logical, 2 * blk);
		btrfs_wib_replace_mark_stale(fs_info, logical + blk, true);
	}
	fs_info->fs_devices->missing_devices = 1;
	btrfs_wib_add_sticky(fs_info, extra, blk);
	btrfs_wib_mark_stale(fs_info, extra, blk);
	fs_info->fs_devices->missing_devices = 0;
	lost = btrfs_wib_replace_marks_lost(fs_info);
	end = btrfs_wib_replace_end(fs_info, true);
	if (evicts) {
		if (lost || end) {
			test_err("raid56_wf_evict_replace_marks is set, yet the replace failed (%d): the test is blind",
				 end);
			goto out;
		}
		test_msg("raid56_wf_evict_replace_marks is set: a replace's mark spent, the replace finished, as expected");
		ret = 0;
		goto out;
	}
	if (!lost || end != -EIO) {
		test_err("a replace whose record of zeros on its target was spent was not failed: lost %d, end %d",
			 lost, end);
		goto out;
	}
	if (btrfs_wib_stale(fs_info, (base + nr - 1) * BTRFS_WIB_ENTRY_SIZE + blk)) {
		test_err("a replace that failed handed its marks over");
		goto out;
	}
	if (atomic64_read(&wib->stat_alert[BTRFS_RAID56_EV_REPLACE_DROPPED]) == alerts0) {
		test_err("no replace_record_dropped alert");
		goto out;
	}
	btrfs_wib_replace_end(fs_info, false);
	if (btrfs_wib_replace_marks_lost(fs_info)) {
		test_err("the end of the failed replace left it failed for the next one");
		goto out;
	}
	ret = 0;
out:
	fs_info->fs_devices->missing_devices = 0;
	btrfs_wib_replace_end(fs_info, false);
	for (u32 i = 0; i <= nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       i == nr ? blk : 2 * blk);
	if (!ret && btrfs_wib_any_stale(fs_info)) {
		test_err("the replace-marks test left something counted stale");
		ret = -EINVAL;
	}
	return ret;
}

/*
 * The same for a record of zeros a replace made under a column that was stale
 * already -- named by a degraded write into it, the usual case with the device
 * missing.  That mark is not the replace's to hide or drop, but the zeros are
 * on the target all the same: a full degraded log spends the record after
 * every other one naming a member, another writer naming the column does not
 * change that, and a replace that lost it anyway fails.  Under
 * raid56_wf_replace_keeps_added_only=1 (or raid56_wf_evict_replace_marks=1)
 * the record in the first slot is spent first, and the replace finishes.
 */
static int test_replace_keeps_stale_column(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool evicts = btrfs_wib_evicts_replace_marks() ||
			    btrfs_wib_replace_keeps_added_only();
	const u64 base = 3900;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	const u64 owned = base * BTRFS_WIB_ENTRY_SIZE;
	const u64 extra = (base + BTRFS_WIB_MAX_ENTRIES) * BTRFS_WIB_ENTRY_SIZE;
	const u32 nr = BTRFS_WIB_MAX_ENTRIES;
	bool lost;
	int ret = -EINVAL;
	int end;

	if (btrfs_wib_keeps_naming_degraded()) {
		test_msg("raid56_keep_naming_degraded is set, skipping the stale-column replace test");
		return 0;
	}
	if (wib->entries[0].bitmap | wib->entries[0].sticky) {
		test_err("the stale-column replace test needs the first slot free");
		return -EINVAL;
	}

	/* A degraded write named the column, then the replace put zeros there. */
	btrfs_wib_add_sticky(fs_info, owned, 2 * blk);
	btrfs_wib_mark_stale(fs_info, owned + blk, blk);
	if (!btrfs_wib_replace_mark_stale(fs_info, owned + blk, true)) {
		test_err("the replace's mark was not kept");
		goto out;
	}
	if (!btrfs_wib_stale(fs_info, owned + blk)) {
		test_err("a replace hid a mark that was there before it");
		goto out;
	}
	for (u32 i = 1; i < nr; i++) {
		const u64 logical = (base + i) * BTRFS_WIB_ENTRY_SIZE;

		btrfs_wib_add_sticky(fs_info, logical, blk);
		btrfs_wib_mark_stale(fs_info, logical, blk);
	}
	fs_info->fs_devices->missing_devices = 1;
	btrfs_wib_add_sticky(fs_info, extra, blk);
	btrfs_wib_mark_stale(fs_info, extra, blk);
	fs_info->fs_devices->missing_devices = 0;
	if (!find_live_entry(wib, extra)) {
		test_err("a full degraded log did not take the new record");
		goto out;
	}
	if (!find_live_entry(wib, owned) != evicts) {
		if (evicts)
			test_err("raid56_wf_replace_keeps_added_only or raid56_wf_evict_replace_marks is set, yet the replace's record under a column stale before it was not spent first: the test is blind");
		else
			test_err("a full degraded log spent a replace's record of zeros under a column stale before it ahead of a record naming a column");
		goto out;
	}
	if (evicts) {
		if (btrfs_wib_replace_marks_lost(fs_info)) {
			test_err("the replace was failed under the knobs: the test is blind");
			goto out;
		}
		test_msg("raid56_wf_replace_keeps_added_only or raid56_wf_evict_replace_marks is set: the record under a column stale before was spent first, as expected");
		ret = 0;
		goto out;
	}
	if (btrfs_wib_replace_marks_lost(fs_info)) {
		test_err("the replace was failed although no mark of its was spent");
		goto out;
	}

	/*
	 * Another write naming the column does not take the zeros off the
	 * target.  Then nothing but the replace's records to spend.
	 */
	btrfs_wib_mark_stale(fs_info, owned + blk, blk);
	for (u32 i = 1; i <= nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE, blk);
	for (u32 i = 1; i < nr; i++) {
		const u64 logical = (base + i) * BTRFS_WIB_ENTRY_SIZE;

		btrfs_wib_add_sticky(fs_info, logical, 2 * blk);
		btrfs_wib_replace_mark_stale(fs_info, logical + blk, true);
	}
	fs_info->fs_devices->missing_devices = 1;
	btrfs_wib_add_sticky(fs_info, extra, blk);
	btrfs_wib_mark_stale(fs_info, extra, blk);
	fs_info->fs_devices->missing_devices = 0;
	if (find_live_entry(wib, owned)) {
		test_err("a full degraded log of a replace's records spent another before the first slot");
		goto out;
	}
	lost = btrfs_wib_replace_marks_lost(fs_info);
	end = btrfs_wib_replace_end(fs_info, true);
	if (!lost || end != -EIO) {
		test_err("a replace whose record of zeros under a column stale before it was spent was not failed: lost %d, end %d",
			 lost, end);
		goto out;
	}
	ret = 0;
out:
	fs_info->fs_devices->missing_devices = 0;
	btrfs_wib_replace_end(fs_info, false);
	for (u32 i = 0; i <= nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       i == nr ? blk : 2 * blk);
	if (!ret && btrfs_wib_any_stale(fs_info)) {
		test_err("the stale-column replace test left something counted stale");
		ret = -EINVAL;
	}
	return ret;
}

/*
 * A replace's hold on a mark it recorded zeros under (@replace_keep) goes with
 * the mark when a write puts the column or the parity right, on its target
 * too: a failed write that names it again later is not the replace's.
 */
static int test_replace_keep_follows_stale(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const u64 stripe = 970ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 col1 = stripe + BTRFS_WIB_BLOCK_SIZE;
	struct btrfs_wib_entry *e;
	int ret = -EINVAL;

	btrfs_wib_add_sticky(fs_info, stripe, 2 * BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_mark_stale(fs_info, col1, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_update_stale_parity(fs_info, stripe, 0, true);
	if (!btrfs_wib_replace_mark_stale(fs_info, col1, true) ||
	    !btrfs_wib_replace_mark_parity(fs_info, stripe, 0, true)) {
		test_err("the replace's marks were not kept");
		goto out;
	}
	btrfs_wib_clear_stale(fs_info, col1, BTRFS_WIB_BLOCK_SIZE, true);
	btrfs_wib_update_stale_parity(fs_info, stripe, 0, false);
	btrfs_wib_mark_stale(fs_info, col1, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_update_stale_parity(fs_info, stripe, 0, true);
	e = find_live_entry(wib, stripe);
	if (!e || !(e->stale & e->sticky)) {
		test_err("the column is not recorded stale again");
		goto out;
	}
	if ((e->stale & e->replace_keep) | (e->stale_par & e->replace_keep_par)) {
		test_err("a mark set again after a write put the column right is held as the replace's");
		goto out;
	}
	ret = 0;
out:
	btrfs_wib_replace_end(fs_info, false);
	btrfs_wib_clear_sticky(fs_info, stripe, 2 * BTRFS_WIB_BLOCK_SIZE);
	if (!ret && btrfs_wib_any_stale(fs_info)) {
		test_err("the replace-keep test left something counted stale");
		ret = -EINVAL;
	}
	return ret;
}

/*
 * A stale parity the recovery finds on a possibly torn stripe it cannot
 * classify -- what may be an earlier mount's verdict, reloaded from a wide
 * block (btrfs_wib_keep_prior_verdict()) -- is spent last by a full log with a
 * device missing, as this mount's verdicts are, and still makes the block
 * wide.  Under raid56_wf_reload_verdicts_plain=1 it is spent in table order,
 * first from the first slot.
 */
static int test_prior_verdict_spent_last(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool plain = btrfs_wib_reload_verdicts_plain();
	const u64 evicted0 = atomic64_read(&wib->stat_sticky_evicted);
	const u64 base = 4000;
	const u64 blk = BTRFS_WIB_BLOCK_SIZE;
	const u64 verdict = base * BTRFS_WIB_ENTRY_SIZE;
	const u32 nr = BTRFS_WIB_MAX_ENTRIES;
	struct btrfs_wib_entry e = { 0 };
	void *block;
	bool kept;
	int ret = -EINVAL;

	if (btrfs_wib_keeps_naming_degraded()) {
		test_msg("raid56_keep_naming_degraded is set, skipping the prior-verdict test");
		return 0;
	}
	if (wib->entries[0].bitmap | wib->entries[0].sticky) {
		test_err("the prior-verdict test needs the first slot free");
		return -EINVAL;
	}
	block = kzalloc(BTRFS_WIB_SLOT_SIZE, GFP_KERNEL);
	if (!block)
		return -ENOMEM;

	/* As the recovery re-adds it (wib_readd_stale()), then keeps it. */
	btrfs_wib_add_sticky(fs_info, verdict, 3 * blk);
	btrfs_wib_update_stale_parity(fs_info, verdict, 0, true);
	btrfs_wib_keep_prior_verdict(fs_info, verdict, BIT(0));
	if (btrfs_wib_build_block(wib, block, 1, NULL) ||
	    !(le64_to_cpu(((struct btrfs_wib_disk_header *)block)->flags) &
	      BTRFS_WIB_FLAG_STALE)) {
		test_err("a reloaded verdict did not keep the block wide");
		goto out;
	}
	btrfs_wib_read_entry(block, 0, &e);
	if (e.bytenr != verdict || e.stale_par != 1) {
		test_err("a wide block did not carry the reloaded verdict: stale_par 0x%llx",
			 e.stale_par);
		goto out;
	}
	for (u32 i = 1; i < nr; i++) {
		const u64 logical = (base + i) * BTRFS_WIB_ENTRY_SIZE;

		btrfs_wib_add_sticky(fs_info, logical, blk);
		btrfs_wib_mark_stale(fs_info, logical, blk);
	}
	if (atomic64_read(&wib->stat_sticky_evicted) != evicted0) {
		test_err("a record was spent before the log was full");
		goto out;
	}
	fs_info->fs_devices->missing_devices = 1;
	btrfs_wib_add_sticky(fs_info, (base + nr) * BTRFS_WIB_ENTRY_SIZE, blk);
	btrfs_wib_mark_stale(fs_info, (base + nr) * BTRFS_WIB_ENTRY_SIZE, blk);
	fs_info->fs_devices->missing_devices = 0;
	kept = find_live_entry(wib, verdict);
	if (atomic64_read(&wib->stat_sticky_evicted) != evicted0 + 1 ||
	    !find_live_entry(wib, (base + nr) * BTRFS_WIB_ENTRY_SIZE)) {
		test_err("a full degraded log spent %llu records to take one",
			 atomic64_read(&wib->stat_sticky_evicted) - evicted0);
		goto out;
	}
	if (kept == plain) {
		if (plain)
			test_err("raid56_wf_reload_verdicts_plain is set, yet the reloaded verdict in the first slot was not spent first: the test is blind");
		else
			test_err("a full degraded log spent a reloaded verdict before a record naming a column");
		goto out;
	}
	if (plain)
		test_msg("raid56_wf_reload_verdicts_plain is set: reloaded verdict spent first, as expected");
	ret = 0;
out:
	fs_info->fs_devices->missing_devices = 0;
	for (u32 i = 0; i <= nr; i++)
		btrfs_wib_clear_sticky(fs_info, (base + i) * BTRFS_WIB_ENTRY_SIZE,
				       i ? blk : 3 * blk);
	if (!ret && btrfs_wib_any_stale(fs_info)) {
		test_err("the prior-verdict test left something counted stale");
		ret = -EINVAL;
	}
	kfree(block);
	return ret;
}

/*
 * A mark is satisfied by the first block written with its sequence number,
 * whoever writes it.  When that is btrfs_wib_persist_now() -- a repair making
 * its write-back durable -- between a transaction commit's snapshot and its
 * drop, the marked write issues its bios while the commit's barrier may be
 * flushing the devices: the commit must keep the stripe listed even if the
 * write finishes before the drop, and must not take the write for flushed.
 * It did both when only the mark's own lazy commit folded the set into the
 * snapshot (raid56_wf_snapshot_misses_marks=1).
 */
static int test_persist_folds_snapshot(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool misses = btrfs_wib_snapshot_misses_marks();
	const u64 stripe = 980ULL * BTRFS_WIB_ENTRY_SIZE;
	unsigned long flags;
	bool written = false;
	bool listed;
	u64 want;
	int ret;

	/* write_all_supers(): the snapshot before the barriers, without the stripe. */
	btrfs_wib_commit_prepare(fs_info);
	/* A write marks the stripe; its lazy commit waits for commit_mutex... */
	spin_lock_irqsave(&wib->lock, flags);
	ret = btrfs_wib_try_mark(wib, stripe, BTRFS_WIB_BLOCK_SIZE);
	want = wib->snap_seq + 1;
	spin_unlock_irqrestore(&wib->lock, flags);
	if (ret) {
		test_err("mark failed: %d", ret);
		return ret;
	}
	/* ...which a repair's persist takes first, writing a block that lists it. */
	ret = btrfs_wib_persist_now(fs_info);
	if (ret) {
		test_err("persist failed: %d", ret);
		goto out_done;
	}
	if (wib->seq < want || !(block_bitmap(wib->last, stripe) & 0x1)) {
		test_err("the persist did not write the mark's record");
		ret = -EINVAL;
		goto out_done;
	}
	/* Satisfied (wib_commit_wait()): the write issues, the barrier runs, it finishes. */
	btrfs_wib_note_written(fs_info, stripe, 1, 0x1, 0, true);
	btrfs_wib_done(fs_info, stripe, BTRFS_WIB_BLOCK_SIZE, false);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	listed = block_bitmap(wib->last, stripe) & 0x1;
	spin_lock_irqsave(&wib->lock, flags);
	for (int i = 0; i < BTRFS_WIB_WRITTEN_SLOTS; i++)
		if (wib->written[i].bytenr == stripe && (wib->written[i].cols & 0x1))
			written = true;
	spin_unlock_irqrestore(&wib->lock, flags);
	if (listed == misses || written == misses) {
		if (misses)
			test_err("raid56_wf_snapshot_misses_marks is set, yet the commit kept what the persist recorded after its snapshot: the test is blind");
		else
			test_err("a commit let go of a write that may have been in flight during its barrier: listed %d, written %d",
				 listed, written);
		return -EINVAL;
	}
	if (misses)
		test_msg("raid56_wf_snapshot_misses_marks is set: stripe dropped, as expected");

	/* The next commit's barrier came after the write: now it goes. */
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_bitmap(wib->last, stripe)) {
		test_err("the stripe outlived a barrier issued after its write");
		return -EINVAL;
	}
	return 0;
out_done:
	btrfs_wib_done(fs_info, stripe, BTRFS_WIB_BLOCK_SIZE, false);
	return ret;
}

/*
 * Writes that finished after the last commit are still listed in flight when
 * the filesystem is unmounted, and read to the next mount as writes a crash
 * may have torn.  btrfs_wib_unmount() has the log say they finished: the
 * finished write drops, the failed one stays an error record, neither stays
 * in flight.  A record the set marks possibly torn stays in flight, and a
 * block that lists nothing else in flight is not written again.  With
 * raid56_wf_finished_stay_inflight=1 the finished writes stay in flight.
 */
static int test_unmount_drops_finished(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool stays = btrfs_wib_finished_stay_inflight();
	const u64 torn_listed = btrfs_wib_torn_no_persist() ? 0 : 0x1;
	const u64 done = 1500ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 failed = 1501ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 torn = 1502ULL * BTRFS_WIB_ENTRY_SIZE;
	struct btrfs_wib_entry *e;
	s64 commits;
	int ret;

	ret = btrfs_wib_mark(fs_info, done, BTRFS_WIB_BLOCK_SIZE);
	if (!ret)
		ret = btrfs_wib_mark(fs_info, failed, BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("mark failed: %d", ret);
		return ret;
	}
	btrfs_wib_done(fs_info, done, BTRFS_WIB_BLOCK_SIZE, false);
	btrfs_wib_done(fs_info, failed, BTRFS_WIB_BLOCK_SIZE, true);
	if (!(block_bitmap(wib->last, done) & 0x1) || !(block_bitmap(wib->last, failed) & 0x1)) {
		test_err("the writes are not listed in flight before the unmount");
		return -EINVAL;
	}
	/* As the recovery leaves a stripe it kept undecided (wib_keep_torn()). */
	btrfs_wib_add_sticky(fs_info, torn, BTRFS_WIB_BLOCK_SIZE);
	e = find_live_entry(wib, torn);
	ret = -EINVAL;
	if (!e) {
		test_err("the possibly torn record is not in the set");
		goto out;
	}
	e->torn = e->sticky;

	btrfs_wib_unmount(fs_info);
	if (block_bitmap(wib->last, done) != (stays ? 0x1 : 0) ||
	    block_bitmap(wib->last, failed) != (stays ? 0x1 : 0)) {
		test_err("after the unmount the block lists in flight 0x%llx and 0x%llx, expected 0x%x",
			 block_bitmap(wib->last, done), block_bitmap(wib->last, failed),
			 stays ? 0x1 : 0);
		goto out;
	}
	if (!stays && block_error(wib->last, done)) {
		test_err("the write that finished became an error record");
		goto out;
	}
	if (!((block_bitmap(wib->last, failed) | block_error(wib->last, failed)) & 0x1)) {
		test_err("the unmount dropped the record of the failed write");
		goto out;
	}
	if (stays) {
		test_msg("%s is set: the finished writes stay in flight, as expected",
			 "raid56_wf_finished_stay_inflight");
		ret = 0;
		goto out;
	}
	if (block_bitmap(wib->last, torn) != torn_listed || !(block_error(wib->last, torn) & 0x1)) {
		test_err("after the unmount the possibly torn record is listed in flight 0x%llx error 0x%llx, expected 0x%llx 0x1",
			 block_bitmap(wib->last, torn), block_error(wib->last, torn), torn_listed);
		goto out;
	}
	commits = atomic64_read(&wib->stat_commits);
	btrfs_wib_unmount(fs_info);
	if (atomic64_read(&wib->stat_commits) != commits) {
		test_err("an unmount with nothing finished left in flight wrote the log again");
		goto out;
	}
	ret = 0;
out:
	btrfs_wib_clear_sticky(fs_info, failed, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_clear_sticky(fs_info, torn, BTRFS_WIB_BLOCK_SIZE);
	btrfs_wib_commit_prepare(fs_info);
	if (btrfs_wib_commit(fs_info, true) && !ret)
		ret = -EINVAL;
	if (!ret && block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the unmount test");
		ret = -EINVAL;
	}
	return ret;
}

/*
 * A remount read-only is an unmount to the log: nothing writes it again before
 * a crash or the next mount reads it, and the unmount of the read-only
 * filesystem writes nothing.  So it drops the writes that finished after the
 * last commit from the listing in flight, as btrfs_wib_unmount() does.
 * raid56_wf_remount_ro_keeps_inflight=1 leaves them listed.
 */
static int test_remount_ro_drops_finished(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool stays = btrfs_wib_remount_ro_keeps_inflight() ||
			   btrfs_wib_finished_stay_inflight();
	const u64 done = 1510ULL * BTRFS_WIB_ENTRY_SIZE;
	int ret;

	ret = btrfs_wib_mark(fs_info, done, BTRFS_WIB_BLOCK_SIZE);
	if (ret) {
		test_err("mark failed: %d", ret);
		return ret;
	}
	btrfs_wib_done(fs_info, done, BTRFS_WIB_BLOCK_SIZE, false);
	if (!(block_bitmap(wib->last, done) & 0x1)) {
		test_err("the write is not listed in flight before the remount");
		return -EINVAL;
	}
	btrfs_wib_remount_ro(fs_info);
	if (block_bitmap(wib->last, done) != (stays ? 0x1 : 0)) {
		test_err("after the remount read-only the block lists in flight 0x%llx, expected 0x%x",
			 block_bitmap(wib->last, done), stays ? 0x1 : 0);
		return -EINVAL;
	}
	if (stays)
		test_msg("finished writes stay in flight: the remount left it listed, as expected");
	btrfs_wib_commit_prepare(fs_info);
	ret = btrfs_wib_commit(fs_info, true);
	if (ret)
		return ret;
	if (block_nr_entries(wib->last) != 0) {
		test_err("log not empty after the remount test");
		return -EINVAL;
	}
	return 0;
}

/*
 * The recovery found a stripe a write may have torn consistent but for a
 * parity on a missing device (btrfs_wib_parity_unwritten()): that parity is
 * recorded stale, and the stripe no longer possibly torn, so the read path
 * does not refuse a rebuild from the parity the recovery regenerated.  With
 * raid56_wf_missing_parity_keeps_torn=1 it stays possibly torn, as before,
 * and no parity is recorded.
 */
static int test_parity_unwritten(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool keeps = btrfs_wib_missing_parity_keeps_torn();
	const u64 start = 1520ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 len = 2 * BTRFS_WIB_BLOCK_SIZE;
	struct btrfs_wib_stripe_state st = { 0 };
	struct btrfs_wib_entry *e;
	bool named;
	bool torn;
	int ret = -EINVAL;

	/* Kept, and marked possibly torn by an earlier pass of the mount. */
	btrfs_wib_add_sticky(fs_info, start, len);
	e = find_live_entry(wib, start);
	if (!e) {
		test_err("the record of the stripe is not in the set");
		goto out;
	}
	e->torn = e->sticky;

	/* Q (parity 1) is on the missing device. */
	btrfs_wib_parity_unwritten(fs_info, start, len, BIT(1));
	torn = btrfs_wib_stripe_torn(fs_info, start, len);
	named = btrfs_wib_stripe_state(fs_info, start, 2, 2, &st);
	if (keeps) {
		if (!torn || (named && st.bad_parity)) {
			test_err("raid56_wf_missing_parity_keeps_torn is set, yet the stripe is torn %d, bad parity 0x%x",
				 torn, named ? st.bad_parity : 0);
			goto out;
		}
		test_msg("%s is set: the stripe stays possibly torn, as expected",
			 "raid56_wf_missing_parity_keeps_torn");
		ret = 0;
		goto out;
	}
	if (torn || !named || st.bad_parity != BIT(1) || st.stale_cols) {
		test_err("the stripe is torn %d, bad parity 0x%x, stale columns 0x%llx, expected 0, 0x2, 0",
			 torn, named ? st.bad_parity : 0, named ? st.stale_cols : 0);
		goto out;
	}
	ret = 0;
out:
	btrfs_wib_clear_sticky(fs_info, start, len);
	if (!ret && (btrfs_wib_stripe_torn(fs_info, start, len) ||
		     btrfs_wib_stripe_state(fs_info, start, 2, 2, &st))) {
		test_err("the record outlived clearing it");
		ret = -EINVAL;
	}
	return ret;
}

/*
 * The recovery decided a RAID6 stripe's data column on a missing device and
 * regenerated both parities (BTRFS_WIB_STRIPE_DECIDED): the stripe is kept
 * recorded for the device, not possibly torn -- among the recovery's kept
 * ones no more either -- and no parity is recorded stale.  With
 * raid56_wf_absent_decided_keeps_torn=1 it stays possibly torn, as before.
 */
static int test_absent_decided(struct btrfs_fs_info *fs_info)
{
	struct btrfs_wib *wib = fs_info->wib;
	const bool keeps = btrfs_wib_absent_decided_keeps_torn();
	const u64 start = 1530ULL * BTRFS_WIB_ENTRY_SIZE;
	const u64 len = 2 * BTRFS_WIB_BLOCK_SIZE;
	struct btrfs_wib_stripe_state st = { 0 };
	struct btrfs_wib_entry *e;
	bool named;
	bool torn;
	int ret = -EINVAL;

	/* Kept, and marked possibly torn by an earlier pass of the mount. */
	btrfs_wib_add_sticky(fs_info, start, len);
	e = find_live_entry(wib, start);
	if (!e) {
		test_err("the record of the stripe is not in the set");
		goto out;
	}
	e->torn = e->sticky;
	e->kept_torn = e->sticky;

	btrfs_wib_parity_unwritten(fs_info, start, len, BTRFS_WIB_STRIPE_DECIDED);
	torn = btrfs_wib_stripe_torn(fs_info, start, len);
	named = btrfs_wib_stripe_state(fs_info, start, 2, 2, &st);
	if (!find_live_entry(wib, start)) {
		test_err("the decided stripe is no longer recorded");
		goto out;
	}
	if (keeps) {
		if (!torn || (named && (st.bad_parity || st.stale_cols))) {
			test_err("raid56_wf_absent_decided_keeps_torn is set, yet the stripe is torn %d, bad parity 0x%x",
				 torn, named ? st.bad_parity : 0);
			goto out;
		}
		test_msg("%s is set: the stripe stays possibly torn, as expected",
			 "raid56_wf_absent_decided_keeps_torn");
		ret = 0;
		goto out;
	}
	if (torn || e->kept_torn || (named && (st.bad_parity || st.stale_cols))) {
		test_err("the decided stripe is torn %d (kept 0x%llx), bad parity 0x%x, stale columns 0x%llx, expected all 0",
			 torn, e->kept_torn, named ? st.bad_parity : 0,
			 named ? st.stale_cols : 0);
		goto out;
	}
	ret = 0;
out:
	btrfs_wib_clear_sticky(fs_info, start, len);
	if (!ret && (btrfs_wib_stripe_torn(fs_info, start, len) ||
		     btrfs_wib_stripe_state(fs_info, start, 2, 2, &st))) {
		test_err("the record outlived clearing it");
		ret = -EINVAL;
	}
	return ret;
}

int btrfs_test_raid56_wib(u32 sectorsize, u32 nodesize)
{
	struct btrfs_fs_info *fs_info;
	int ret;

	test_msg("running raid56 write-intent log tests");

	fs_info = btrfs_alloc_dummy_fs_info(nodesize, sectorsize);
	if (!fs_info) {
		test_std_err(TEST_ALLOC_FS_INFO);
		return -ENOMEM;
	}
	fs_info->csum_type = BTRFS_CSUM_TYPE_CRC32;
	memcpy(fs_info->fs_devices->metadata_uuid, test_uuid, BTRFS_FSID_SIZE);
	mutex_init(&fs_info->fs_devices->device_list_mutex);
	fs_info->super_for_commit = kzalloc_obj(struct btrfs_super_block);
	if (!fs_info->super_for_commit) {
		ret = -ENOMEM;
		goto out;
	}

	ret = btrfs_wib_alloc(fs_info);
	if (ret) {
		test_err("failed to allocate the log: %d", ret);
		goto out;
	}
	/* No device to repair on: a readd may ask, nothing runs. */
	fs_info->wib->repair_paused = true;

	ret = test_range_mask();
	if (ret)
		goto out;
	ret = test_mark_commit_done(fs_info);
	if (ret)
		goto out;
	ret = test_torn_block(fs_info);
	if (ret)
		goto out;
	ret = test_pending_merge(fs_info);
	if (ret)
		goto out;
	ret = test_unrecovered(fs_info);
	if (ret)
		goto out;
	ret = test_recovery_takes_stripes(fs_info);
	if (ret)
		goto out;
	ret = test_unrecovered_health(fs_info);
	if (ret)
		goto out;
	ret = test_unmarked_log(fs_info);
	if (ret)
		goto out;
	ret = test_log_full(fs_info);
	if (ret)
		goto out;
	ret = test_sticky(fs_info);
	if (ret)
		goto out;
	ret = test_capacity_follows_layout(fs_info);
	if (ret)
		goto out;
	ret = test_suspect_narrow(fs_info);
	if (ret)
		goto out;
	ret = test_suspect_spent_last(fs_info);
	if (ret)
		goto out;
	ret = test_suspect_replace_end(fs_info);
	if (ret)
		goto out;
	ret = test_replace_marks_spent_last(fs_info);
	if (ret)
		goto out;
	ret = test_replace_keeps_stale_column(fs_info);
	if (ret)
		goto out;
	ret = test_replace_keep_follows_stale(fs_info);
	if (ret)
		goto out;
	ret = test_prior_verdict_spent_last(fs_info);
	if (ret)
		goto out;
	ret = test_evict_precedence(fs_info);
	if (ret)
		goto out;
	ret = test_named_records_stay(fs_info);
	if (ret)
		goto out;
	ret = test_torn_spent_last(fs_info);
	if (ret)
		goto out;
	ret = test_torn_waits_for_write(fs_info);
	if (ret)
		goto out;
	ret = test_kept_torn_spent_last(fs_info);
	if (ret)
		goto out;
	ret = test_readd_keeps_records(fs_info);
	if (ret)
		goto out;
	ret = test_readd_wait_bounded(fs_info);
	if (ret)
		goto out;
	ret = test_readd_keeps_verdicts(fs_info);
	if (ret)
		goto out;
	ret = test_readd_busy_region(fs_info);
	if (ret)
		goto out;
	ret = test_readd_names(fs_info);
	if (ret)
		goto out;
	ret = test_stale_parity_only(fs_info);
	if (ret)
		goto out;
	ret = test_persist_folds_snapshot(fs_info);
	if (ret)
		goto out;
	ret = test_unmount_drops_finished(fs_info);
	if (ret)
		goto out;
	ret = test_remount_ro_drops_finished(fs_info);
	if (ret)
		goto out;
	ret = test_parity_unwritten(fs_info);
	if (ret)
		goto out;
	ret = test_absent_decided(fs_info);
	if (ret)
		goto out;
	ret = test_forget_range(fs_info);
	if (ret)
		goto out;
	ret = test_replace_marks(fs_info);
	if (ret)
		goto out;
	ret = test_disable_ordering(fs_info);
out:
	kfree(fs_info->super_for_commit);
	fs_info->super_for_commit = NULL;
	btrfs_free_dummy_fs_info(fs_info);
	return ret;
}
