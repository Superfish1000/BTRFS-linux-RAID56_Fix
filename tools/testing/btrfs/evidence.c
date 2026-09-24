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
 *   evidence <mountpoint> stats            counters only, ring left alone
 *   evidence <mountpoint> race <seconds>   arm/disarm/read as fast as possible
 *   evidence <mountpoint> collect <outdir> [until-pid]
 *                                           the whole job in one process: arm
 *                                           bound to this process, drain while
 *                                           the scrub runs, and disarm only
 *                                           once nothing is left
 *   evidence <mountpoint> hold             arm bound to this process and never
 *                                           read -- what a helper that hangs or
 *                                           gets killed looks like to the kernel
 *   evidence <mountpoint> disarm-if-empty  disarm unless something is queued
 *
 * collect is the one to use.  The separate arm/drain/disarm commands leave the
 * channel's lifetime to whoever runs them; collect ties it to this process
 * (BTRFS_RAID56_EVIDENCE_ARM_BIND), so if it is killed the kernel disarms at
 * once and says how many captured stripes that threw away, instead of leaving
 * the ring to fill with nobody reading it.
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
#include <time.h>
#include <signal.h>

#define BTRFS_IOCTL_MAGIC 0x94
#define BTRFS_RAID56_EVIDENCE_ARM	0
#define BTRFS_RAID56_EVIDENCE_READ	1
#define BTRFS_RAID56_EVIDENCE_DISARM	2
#define BTRFS_RAID56_EVIDENCE_ARM_BIND		(1ULL << 0)
#define BTRFS_RAID56_EVIDENCE_DISARM_IF_EMPTY	(1ULL << 1)
#define MAX_COLS 34

struct ev_args {
	uint64_t op, flags, full_stripe_start, gen, record_flags, stale_cols, bad_parity;
	uint32_t nr_data, nr_parity, stripe_len, nr_queued;
	uint64_t dropped_full, dropped_wide, captured, waited;
	uint64_t reserved[8];
	uint64_t devid[MAX_COLS], physical[MAX_COLS];
	uint64_t buf_size;
	uint8_t buf[];
};
#define BTRFS_IOC_RAID56_EVIDENCE _IOWR(BTRFS_IOCTL_MAGIC, 68, struct ev_args)

#define CAP (16 * 65536)

/* Returns 0 or the errno, so callers can tell EBUSY and EINVAL apart. */
static int op_flags(int fd, uint64_t op, uint64_t flags)
{
	struct ev_args a;

	memset(&a, 0, sizeof(a));
	a.op = op;
	a.flags = flags;
	return ioctl(fd, BTRFS_IOC_RAID56_EVIDENCE, &a) < 0 ? errno : 0;
}

static int simple(int fd, uint64_t op)
{
	int err = op_flags(fd, op, 0);

	if (err) {
		fprintf(stderr, "EVIDENCE op %llu: %s\n",
			(unsigned long long)op, strerror(err));
		return 1;
	}
	return 0;
}

/* Drain everything queued now.  Returns how many it took, or -1 on error. */
static int drain_quiet(int fd, const char *outdir, int verbose);

static int drain(int fd, const char *outdir)
{
	int n = drain_quiet(fd, outdir, 1);

	if (n < 0)
		return 1;
	printf("EVIDENCE_DRAINED %d\n", n);
	return 0;
}

static int drain_quiet(int fd, const char *outdir, int verbose)
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
			return -1;
		}
		snprintf(path, sizeof(path), "%s/stripe-%llu.data", outdir,
			 (unsigned long long)a->full_stripe_start);
		out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (out < 0 || write(out, a->buf, a->buf_size) != (ssize_t)a->buf_size) {
			fprintf(stderr, "write %s: %s\n", path, strerror(errno));
			free(a);
			return -1;
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
		if (verbose)
			printf("EVIDENCE stripe %llu data=%llu queued=%u captured=%llu dropped_full=%llu dropped_wide=%llu\n",
			       (unsigned long long)a->full_stripe_start,
			       (unsigned long long)a->buf_size, a->nr_queued,
			       (unsigned long long)a->captured,
			       (unsigned long long)a->dropped_full,
			       (unsigned long long)a->dropped_wide);
		if (!a->nr_queued)
			break;
	}
	free(a);
	return n;
}

/*
 * The counters, without draining anything.  A READ on an empty ring returns
 * -ENOENT and still fills the header, which is the only way to ask "how much
 * did you have to throw away" without consuming what is queued.
 */
static int stats(int fd)
{
	struct ev_args *a = calloc(1, sizeof(*a) + CAP);
	int ret = 0;

	if (!a)
		return 1;
	a->op = BTRFS_RAID56_EVIDENCE_READ;
	a->buf_size = 0;		/* want nothing back but the header */
	if (ioctl(fd, BTRFS_IOC_RAID56_EVIDENCE, a) < 0 &&
	    errno != ENOENT && errno != ERANGE) {
		fprintf(stderr, "STATS: %s\n", strerror(errno));
		ret = 1;
	} else {
		printf("EVIDENCE_STATS captured=%llu dropped_full=%llu dropped_wide=%llu waited=%llu queued=%u\n",
		       (unsigned long long)a->captured,
		       (unsigned long long)a->dropped_full,
		       (unsigned long long)a->dropped_wide,
		       (unsigned long long)a->waited, a->nr_queued);
	}
	free(a);
	return ret;
}

/*
 * Hammer arm, read and disarm for @secs seconds.
 *
 * The window this is aiming at is between a capture claiming a slot and
 * publishing it: a disarm landing there used to clear the pointer, block on
 * the channel's own lock waiting for the capture to finish, and then never be
 * woken, because the capture saw a NULL pointer and returned without
 * releasing that lock.  The same pointer read could also outlive the object it
 * pointed at.  Neither is visible from userspace as anything but a hang or a
 * splat, so this does not check a return value -- the test is that the scrub
 * running alongside it finishes and the kernel logs nothing.
 */
static int race(int fd, int secs)
{
	struct ev_args *a = calloc(1, sizeof(*a) + CAP);
	time_t end = time(NULL) + secs;
	unsigned long n = 0;

	if (!a)
		return 1;
	while (time(NULL) < end) {
		memset(a, 0, sizeof(*a));
		a->op = BTRFS_RAID56_EVIDENCE_ARM;
		ioctl(fd, BTRFS_IOC_RAID56_EVIDENCE, a);

		memset(a, 0, sizeof(*a));
		a->op = BTRFS_RAID56_EVIDENCE_READ;
		a->buf_size = CAP;
		ioctl(fd, BTRFS_IOC_RAID56_EVIDENCE, a);

		memset(a, 0, sizeof(*a));
		a->op = BTRFS_RAID56_EVIDENCE_DISARM;
		ioctl(fd, BTRFS_IOC_RAID56_EVIDENCE, a);
		n++;
	}
	printf("EVIDENCE_RACE_ROUNDS %lu\n", n);
	free(a);
	return 0;
}

static volatile sig_atomic_t stop_requested;

static void on_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static int pid_gone(pid_t pid)
{
	return pid > 0 && kill(pid, 0) < 0 && errno == ESRCH;
}

/*
 * The whole job, in one process, so that the channel's lifetime is this
 * process's lifetime.
 *
 * Runs until told to stop (SIGINT/SIGTERM) or until @until -- normally the
 * scrub -- exits.  Then it does not simply disarm: a capture can land between
 * the last READ and the DISARM and would be thrown away, so it asks for
 * DISARM_IF_EMPTY and goes back to reading whenever the kernel says -EBUSY.
 */
static int collect(int fd, const char *outdir, pid_t until)
{
	int bound = 1, total = 0, n, err;

	err = op_flags(fd, BTRFS_RAID56_EVIDENCE_ARM, BTRFS_RAID56_EVIDENCE_ARM_BIND);
	if (err == EINVAL) {
		/* A kernel without ARM_BIND: still collect, but say what is lost. */
		bound = 0;
		fprintf(stderr, "EVIDENCE kernel has no ARM_BIND: if this process dies the channel stays armed\n");
		err = op_flags(fd, BTRFS_RAID56_EVIDENCE_ARM, 0);
	}
	if (err) {
		fprintf(stderr, "ARM: %s\n", strerror(err));
		return 1;
	}
	printf("EVIDENCE_COLLECTING bound=%d\n", bound);
	fflush(stdout);

	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	for (;;) {
		n = drain_quiet(fd, outdir, 1);
		if (n < 0)
			return 1;
		total += n;
		if (stop_requested || pid_gone(until))
			break;
		/* Only pause when there was nothing to take: captures burst. */
		if (!n)
			usleep(100000);
	}

	/* What the kernel had to discard, if anything, before letting go. */
	stats(fd);
	for (;;) {
		n = drain_quiet(fd, outdir, 1);
		if (n < 0)
			return 1;
		total += n;
		err = op_flags(fd, BTRFS_RAID56_EVIDENCE_DISARM,
			       BTRFS_RAID56_EVIDENCE_DISARM_IF_EMPTY);
		if (!err)
			break;
		if (err == EBUSY)
			continue;	/* something landed; read it and ask again */
		if (err == EINVAL) {
			/* No DISARM_IF_EMPTY: the best available is one last drain. */
			n = drain_quiet(fd, outdir, 1);
			total += n > 0 ? n : 0;
			op_flags(fd, BTRFS_RAID56_EVIDENCE_DISARM, 0);
			break;
		}
		fprintf(stderr, "DISARM: %s\n", strerror(err));
		return 1;
	}
	printf("EVIDENCE_COLLECTED %d\n", total);
	return 0;
}

/* Arm bound to this process and never read.  For tests to kill. */
static int hold(int fd)
{
	int err = op_flags(fd, BTRFS_RAID56_EVIDENCE_ARM, BTRFS_RAID56_EVIDENCE_ARM_BIND);

	if (err) {
		fprintf(stderr, "ARM_BIND: %s\n", strerror(err));
		return 1;
	}
	printf("EVIDENCE_HOLDING\n");
	fflush(stdout);
	for (;;)
		pause();
}

static int disarm_if_empty(int fd)
{
	int err = op_flags(fd, BTRFS_RAID56_EVIDENCE_DISARM,
			   BTRFS_RAID56_EVIDENCE_DISARM_IF_EMPTY);

	if (!err)
		printf("EVIDENCE_DISARM_IF_EMPTY ok\n");
	else if (err == EBUSY)
		printf("EVIDENCE_DISARM_IF_EMPTY busy\n");
	else
		printf("EVIDENCE_DISARM_IF_EMPTY error %s\n", strerror(err));
	return err && err != EBUSY;
}

int main(int argc, char **argv)
{
	int fd, ret;

	if (argc < 3) {
		fprintf(stderr,
			"usage: %s <mountpoint> arm|drain <dir>|disarm|stats|race <secs>|"
			"collect <dir> [until-pid]|hold|disarm-if-empty\n",
			argv[0]);
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
	else if (!strcmp(argv[2], "stats"))
		ret = stats(fd);
	else if (!strcmp(argv[2], "race") && argc >= 4)
		ret = race(fd, atoi(argv[3]));
	else if (!strcmp(argv[2], "collect") && argc >= 4)
		ret = collect(fd, argv[3], argc >= 5 ? (pid_t)atoi(argv[4]) : 0);
	else if (!strcmp(argv[2], "hold"))
		ret = hold(fd);
	else if (!strcmp(argv[2], "disarm-if-empty"))
		ret = disarm_if_empty(fd);
	else {
		fprintf(stderr, "unknown command\n");
		ret = 2;
	}
	close(fd);
	return ret;
}
