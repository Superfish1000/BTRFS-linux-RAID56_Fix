// SPDX-License-Identifier: GPL-2.0
/*
 * Report whether a file's data is checksummed, via BTRFS_IOC_GET_CSUMS.
 *
 * Written to settle one question: a file that reads back COMPLETE, at the
 * right length, with content nobody wrote, and with no read error, is only
 * possible if nothing verified it.  Either the range carries no checksum, or
 * the read path did not consult one.  This says which.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BTRFS_IOCTL_MAGIC 0x94
#define HAS_CSUMS   (1U << 0)
#define ZEROED      (1U << 1)
#define NODATASUM   (1U << 2)
#define COMPRESSED  (1U << 3)

struct entry { uint64_t offset, length; uint32_t type, reserved; };
struct args { uint64_t offset, length, buf_size, flags; uint8_t buf[]; };
#define BTRFS_IOC_GET_CSUMS _IOWR(BTRFS_IOCTL_MAGIC, 66, struct args)

#define CAP (64 * 1024)

int main(int argc, char **argv)
{
	struct args *a;
	struct stat st;
	int fd, has = 0, nosum = 0, zero = 0, other = 0;

	if (argc < 2) { fprintf(stderr, "usage: csummap <file>\n"); return 2; }
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return 1; }
	if (fstat(fd, &st)) { fprintf(stderr, "stat: %s\n", strerror(errno)); return 1; }

	a = calloc(1, sizeof(*a) + CAP);
	if (!a) return 1;
	a->offset = 0;
	a->length = st.st_size;
	a->buf_size = CAP;
	if (ioctl(fd, BTRFS_IOC_GET_CSUMS, a) < 0) {
		printf("CSUMMAP %s ioctl-failed %s\n", argv[1], strerror(errno));
		return 1;
	}
	for (uint64_t off = 0; off + sizeof(struct entry) <= a->buf_size; ) {
		struct entry e;

		memcpy(&e, a->buf + off, sizeof(e));
		if (!e.length)
			break;
		if (e.type & HAS_CSUMS) has += 1;
		else if (e.type & NODATASUM) nosum += 1;
		else if (e.type & ZEROED) zero += 1;
		else other += 1;
		off += sizeof(e);
		if (e.type & HAS_CSUMS)
			off += (e.length >> 12) * 4;	/* crc32c, 4K sectors */
	}
	printf("CSUMMAP %s size=%lld ranges: has_csums=%d nodatasum=%d zeroed=%d other=%d\n",
	       argv[1], (long long)st.st_size, has, nosum, zero, other);
	free(a);
	close(fd);
	return 0;
}
