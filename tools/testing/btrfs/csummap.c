// SPDX-License-Identifier: GPL-2.0
/*
 * Report a file's stored checksums, via BTRFS_IOC_GET_CSUMS, and compare each
 * one against the bytes the filesystem actually hands back for that sector.
 *
 * Written to settle one question: a file that reads back COMPLETE, at the
 * right length, with content nobody wrote, and with no read error, is only
 * possible if nothing verified it -- OR if what is stored alongside the data
 * describes the wrong content.  Printing both numbers says which:
 *
 *   stored == actual		the read path is innocent; the csum tree
 *				agrees with the bytes on disk, so whatever
 *				wrote them checksummed what it wrote.  If the
 *				bytes are zeros, a zero page was checksummed.
 *   stored == crc32c(zeros)	same conclusion, stated directly, and it does
 *				not depend on the read succeeding.
 *   stored == 00000000		no csum item covers this sector at all (the
 *				kernel calls that a "csum hole"), and the
 *				buffer we handed the ioctl was never written.
 *   stored != actual		the read path returned data it did not verify.
 *
 * crc32c here is the bare Castagnoli CRC, which is what btrfs stores: the
 * kernel's shash starts at ~0 and complements the result, i.e. the standard
 * CRC-32C of the sector.
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

#define CAP		(64 * 1024)
#define SECTORSIZE	4096

static uint32_t crc32c_tab[256];

static void crc32c_init(void)
{
	for (unsigned int i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0x82F63B78U : c >> 1;
		crc32c_tab[i] = c;
	}
}

static uint32_t crc32c(const uint8_t *p, size_t len)
{
	uint32_t c = ~0U;

	while (len--)
		c = crc32c_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}

/*
 * The ioctl stores each sector's csum little-endian, csum_size bytes wide.  We
 * only ever build crc32c filesystems here, so four bytes.
 */
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
	uint8_t sector[SECTORSIZE], zeros[SECTORSIZE];
	uint32_t zerocsum;
	struct args *a;
	struct stat st;
	int fd, has = 0, nosum = 0, zero = 0, other = 0;
	int mismatch = 0, holes = 0, zerosum = 0, unreadable = 0;

	if (argc < 2) { fprintf(stderr, "usage: csummap <file>\n"); return 2; }
	crc32c_init();
	memset(zeros, 0, sizeof(zeros));
	zerocsum = crc32c(zeros, sizeof(zeros));

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
		printf("CSUMENT %s off=%llu len=%llu type=0x%x%s%s%s%s\n",
		       argv[1], (unsigned long long)e.offset,
		       (unsigned long long)e.length, e.type,
		       (e.type & HAS_CSUMS) ? " HAS_CSUMS" : "",
		       (e.type & NODATASUM) ? " NODATASUM" : "",
		       (e.type & ZEROED) ? " ZEROED" : "",
		       (e.type & COMPRESSED) ? " COMPRESSED" : "");
		if (e.type & HAS_CSUMS) has += 1;
		else if (e.type & NODATASUM) nosum += 1;
		else if (e.type & ZEROED) zero += 1;
		else other += 1;
		off += sizeof(e);
		if (e.type & HAS_CSUMS) {
			const uint8_t *sums = a->buf + off;
			uint64_t nsec = e.length / SECTORSIZE;

			for (uint64_t s = 0; s < nsec; s++) {
				uint64_t foff = e.offset + s * SECTORSIZE;
				uint32_t stored = le32(sums + s * 4);
				ssize_t got;
				const char *state;
				char actual[16] = "unreadable";

				/*
				 * Read the sector the same way anything else
				 * would.  A read error here is the checksum
				 * doing its job and is itself an answer.
				 */
				got = pread(fd, sector, SECTORSIZE, foff);
				if (got == SECTORSIZE) {
					uint32_t ac = crc32c(sector, SECTORSIZE);

					snprintf(actual, sizeof(actual), "%08x", ac);
					if (ac == stored)
						state = "MATCH";
					else
						state = "MISMATCH", mismatch++;
				} else {
					state = "EIO";
					unreadable++;
				}
				if (stored == 0)
					holes++;
				else if (stored == zerocsum)
					zerosum++;
				printf("CSUMSEC %s sec=%llu off=%llu stored=%08x actual=%s %s%s\n",
				       argv[1], (unsigned long long)(foff / SECTORSIZE),
				       (unsigned long long)foff, stored, actual, state,
				       stored == 0 ? " CSUM_HOLE" :
				       stored == zerocsum ? " STORED_IS_ZEROS" : "");
			}
			off += nsec * 4;
		}
	}
	printf("CSUMMAP %s size=%lld ranges: has_csums=%d nodatasum=%d zeroed=%d other=%d "
	       "sectors: mismatch=%d csum_holes=%d stored_is_zeros=%d eio=%d zerocsum=%08x\n",
	       argv[1], (long long)st.st_size, has, nosum, zero, other,
	       mismatch, holes, zerosum, unreadable, zerocsum);
	free(a);
	close(fd);
	return 0;
}
