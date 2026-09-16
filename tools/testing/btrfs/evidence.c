// SPDX-License-Identifier: GPL-2.0
/*
 * Reference consumer for BTRFS_IOC_RAID56_EVIDENCE.
 *
 * The kernel keeps a small ring of full stripes a scrub declined to repair,
 * captured at the verdict from the buffers the scrub already held.  This drains
 * that ring to a directory -- the "recovery drive" -- while the scrub is still
 * running, which is the whole point: waiting until the scrub finishes means
 * waiting for a ring of four slots to have overflowed.
 *
 *   evidence <mountpoint> arm
 *   evidence <mountpoint> drain <outdir>
 *   evidence <mountpoint> disarm
 *
 * Only the DATA columns are in the payload.  The parity is named, not copied:
 * every column's devid and physical offset is in the header, and the block
 * group stays read-only until that chunk's scrub ends, so the parity can be
 * read off the device without racing anything.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

#define BTRFS_IOCTL_MAGIC 0x94
#define BTRFS_RAID56_EVIDENCE_ARM	0
#define BTRFS_RAID56_EVIDENCE_READ	1
#define BTRFS_RAID56_EVIDENCE_DISARM	2
#define MAX_COLS 34

struct ev_args {
	uint64_t op, flags, full_stripe_start, gen, record_flags, stale_cols, bad_parity;
	uint32_t nr_data, nr_parity, stripe_len, nr_queued;
	uint64_t dropped_full, dropped_wide, captured;
	uint64_t devid[MAX_COLS], physical[MAX_COLS];
	uint64_t buf_size;
	uint8_t buf[];
};
#define BTRFS_IOC_RAID56_EVIDENCE _IOWR(BTRFS_IOCTL_MAGIC, 68, struct ev_args)

#define CAP (16 * 65536)

static int simple(int fd, uint64_t op)
{
	struct ev_args a;

	memset(&a, 0, sizeof(a));
	a.op = op;
	if (ioctl(fd, BTRFS_IOC_RAID56_EVIDENCE, &a) < 0) {
		fprintf(stderr, "EVIDENCE op %llu: %s\n",
			(unsigned long long)op, strerror(errno));
		return 1;
	}
	return 0;
}

static int drain(int fd, const char *outdir)
{
	struct ev_args *a = calloc(1, sizeof(*a) + CAP);
	int n = 0;

	if (!a)
		return 1;
	mkdir(outdir, 0700);
	for (;;) {
		char path[512];
		FILE *f;
		int out;

		memset(a, 0, sizeof(*a));
		a->op = BTRFS_RAID56_EVIDENCE_READ;
		a->buf_size = CAP;
		if (ioctl(fd, BTRFS_IOC_RAID56_EVIDENCE, a) < 0) {
			if (errno == ENOENT)
				break;			/* queue empty */
			if (errno == ERANGE) {
				fprintf(stderr, "stripe needs %llu bytes, have %d\n",
					(unsigned long long)a->buf_size, CAP);
				break;
			}
			fprintf(stderr, "READ: %s\n", strerror(errno));
			free(a);
			return 1;
		}
		snprintf(path, sizeof(path), "%s/stripe-%llu.data", outdir,
			 (unsigned long long)a->full_stripe_start);
		out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (out < 0 || write(out, a->buf, a->buf_size) != (ssize_t)a->buf_size) {
			fprintf(stderr, "write %s: %s\n", path, strerror(errno));
			free(a);
			return 1;
		}
		close(out);

		snprintf(path, sizeof(path), "%s/stripe-%llu.meta", outdir,
			 (unsigned long long)a->full_stripe_start);
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "full_stripe_start %llu\ngen %llu\n",
				(unsigned long long)a->full_stripe_start,
				(unsigned long long)a->gen);
			fprintf(f, "nr_data %u\nnr_parity %u\nstripe_len %u\n",
				a->nr_data, a->nr_parity, a->stripe_len);
			fprintf(f, "coherent %s\n",
				(a->record_flags & 1) ? "yes" :
				"NO -- captured by mount recovery without a block group hold");
			fprintf(f, "stale_cols 0x%llx\nbad_parity 0x%llx\n",
				(unsigned long long)a->stale_cols,
				(unsigned long long)a->bad_parity);
			fprintf(f, "data_bytes %llu\n",
				(unsigned long long)a->buf_size);
			for (unsigned i = 0; i < a->nr_data + a->nr_parity; i++)
				fprintf(f, "col %u %s devid %llu physical %llu\n",
					i, i < a->nr_data ? "data  " : "parity",
					(unsigned long long)a->devid[i],
					(unsigned long long)a->physical[i]);
			fclose(f);
		}
		n++;
		printf("EVIDENCE stripe %llu data=%llu queued=%u captured=%llu dropped_full=%llu dropped_wide=%llu\n",
		       (unsigned long long)a->full_stripe_start,
		       (unsigned long long)a->buf_size, a->nr_queued,
		       (unsigned long long)a->captured,
		       (unsigned long long)a->dropped_full,
		       (unsigned long long)a->dropped_wide);
		if (!a->nr_queued)
			break;
	}
	printf("EVIDENCE_DRAINED %d\n", n);
	free(a);
	return 0;
}

int main(int argc, char **argv)
{
	int fd, ret;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <mountpoint> arm|drain <dir>|disarm\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	if (!strcmp(argv[2], "arm"))
		ret = simple(fd, BTRFS_RAID56_EVIDENCE_ARM);
	else if (!strcmp(argv[2], "disarm"))
		ret = simple(fd, BTRFS_RAID56_EVIDENCE_DISARM);
	else if (!strcmp(argv[2], "drain") && argc >= 4)
		ret = drain(fd, argv[3]);
	else {
		fprintf(stderr, "unknown command\n");
		ret = 2;
	}
	close(fd);
	return ret;
}
