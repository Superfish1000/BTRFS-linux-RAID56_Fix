#!/bin/bash
# Runs as init inside UML (host filesystem as root).  Parameters arrive as
# environment variables from the kernel command line:
#   MODE     scenario, see the case statement
#   OPTS     extra mount options (comma separated, may be empty -> "rw")
#   PROFILE  data:metadata profiles, e.g. raid5:raid1
#   CRASH    crash injection point for MODE=prepare (1 or 2)
#   CONVERT  data:metadata profiles to convert to before the crash (prepare)
#   FAIL     device index to fail with device-mapper (detach/flakey modes)
#   TAG      result file suffix
#   MNTDEV   device to mount (default /dev/ubda)
#   NDEV     number of devices in the array
#   OMITTED  device omitted by the host (verify mode)
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
export PATH=$T/progs-install/bin:/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
RES=$T/umltest/results.$TAG
MNT=/mnt/umltest
mkdir -p $MNT
log() { echo "TEST[$MODE]: $*"; echo "[$MODE] $*" >> $RES; }
kmsg() { dmesg | grep -E "$1" | tail -n ${2:-4} | while read -r l; do log "dmesg: $l"; done; }
finish() {
	sync
	dmesg | grep -E "^\[ *[0-9.]+\] (BUG:|WARNING:|KASAN|INFO: task|Oops)|possible circular|lockdep" && log "KERNEL_SPLAT"
	echo o > /proc/sysrq-trigger
	sleep 60
}
# Dump blocked tasks to the console if the scenario takes too long.
watchdog() {
	( sleep ${1:-120}; echo 8 > /proc/sys/kernel/printk; echo "WATCHDOG: dumping blocked tasks";
	  echo w > /proc/sysrq-trigger; sleep 2
	  # UML's sysrq-w often prints an empty call trace; say where each
	  # blocked task is waiting from /proc as well.
	  # Every task that is not simply sleeping, not only D: a hang with
	  # nothing blocked is something spinning.
	  # Threads, not only thread-group leaders: a process blocked in a
	  # worker thread (mkfs.btrfs's TRIM threads) shows only its leader.
	  for d in /proc/[0-9]*/task/[0-9]*; do
		st=$(sed -n 's/^State:[[:space:]]*\(.\).*/\1/p' $d/status 2>/dev/null)
		case "$st" in I|"") continue;; esac
		# A sleeping kernel thread is normal; a sleeping user process
		# is what a hang with nothing running is made of.
		# (procfs files report size 0, so test the content, not -s.)
		[ "$st" = S ] && [ -z "$(tr -d '\0' < $d/cmdline 2>/dev/null)" ] && continue
		echo "WATCHDOG: $st $(cat $d/comm 2>/dev/null) pid ${d#/proc/} wchan $(cat $d/wchan 2>/dev/null) cmd $(tr '\0' ' ' < $d/cmdline 2>/dev/null | cut -c1-80)"
		sed 's/^/WATCHDOG:   /' $d/stack 2>/dev/null | head -20
	  done
	  echo l > /proc/sysrq-trigger; sleep 1
	  echo "WATCHDOG: done" ) &
}
OPTS=${OPTS:-rw}
MNTDEV=${MNTDEV:-/dev/ubda}
DPROF=${PROFILE%%:*}
MPROF=${PROFILE##*:}
btrfs device scan >/dev/null 2>&1
DEVS=$(ls /dev/ubd[a-z] 2>/dev/null | tr '\n' ' ')
log "devices present: $DEVS"
stats() {
	for f in /sys/fs/btrfs/*/raid56_write_intent; do
		[ -f $f ] && log "${1:-sysfs}: $(tr '\n' ' ' < $f)"
	done
	for f in /sys/fs/btrfs/*/raid56_write_profile; do
		[ -f $f ] && log "${1:-sysfs}: $(tr '\n' ' ' < $f)"
	done
}
do_mount() {
	mount -o "$1" $2 $MNT && return
	if dmesg | grep -q "log replay failed"; then
		# A log tree block hit two faults in one vertical stripe (a failed
		# write on the flaky device and the torn parity of the crashed
		# RMW): beyond RAID5 tolerance, detected.  Discard the log like an
		# administrator would and continue with the committed data.
		log "REPLAY_FAILED_DETECTED (two faults in one vertical stripe)"
		kmsg "log replay failed|bad tree block" 3
		btrfs rescue zero-log $2 >/dev/null 2>&1 && log "LOG_ZEROED"
		rm -f $T/umltest/manifest.$TAG; log "MANIFEST_SKIPPED_LOG_DISCARDED"
		mount -o "$1" $2 $MNT && return
	fi
	log "MOUNT_FAIL($1 $2)"; kmsg "BTRFS" 6; finish
}
verify_manifest() {
	# Every file whose fsync returned before the crash, name + md5.
	[ -f $T/umltest/manifest.$TAG ] || return 0
	local total=0 bad=0 srcok=0 silent=0 f m sz srcmd got gotsz
	while read -r f m sz srcmd; do
		total=$((total+1))
		got=$(md5sum $f 2>/dev/null | awk '{print $1}')
		if [ -n "$got" ] && [ -n "$srcmd" ] && [ "$got" = "$srcmd" ] && [ "$got" != "$m" ]; then
			# Reads back as what was WRITTEN.  The manifest's
			# read-back digest was taken through a degraded path
			# and is the thing that was wrong.
			srcok=$((srcok+1))
			log "$1_SRCOK $f readback-digest was wrong, data is what dd wrote"
			continue
		fi
		if [ "$got" != "$m" ]; then
			bad=$((bad+1))
			# Size as well as hash.  A file that reads back COMPLETE
			# with different content and a file that reads back
			# SHORT are different findings, and a hash alone cannot
			# tell them apart -- md5sum is perfectly happy to hash a
			# truncated file and print a valid, different digest.
			gotsz=$(stat -c %s "$f" 2>/dev/null || echo "?")
			# The distinction that matters.  A read that FAILS is
			# the checksum working: the caller is told the content
			# is unavailable and no data is lost that was not
			# already lost.  A read that SUCCEEDS with content
			# nobody wrote is silent corruption -- the caller has
			# no way to know.  Only the second is a defect in the
			# read path, so count it on its own.
			[ -n "$got" ] && [ "$gotsz" = "${sz:-?}" ] && silent=$((silent+1))
			log "$1_BAD $f expected $m got ${got:-READFAIL} size ${gotsz} expected_size ${sz:-?}"
			# A file that reads back COMPLETE with novel content and
			# no error should be impossible: plain dd means the data
			# is checksummed, and a wrong reconstruction must fail
			# that checksum.  One of those is untrue.  Report the
			# things that would say which, for the readable case
			# only -- a READFAIL is the checksum doing its job.
			if [ -n "$got" ]; then
				log "$1_BADINFO $f attr=[$(lsattr -d "$f" 2>/dev/null | awk '{print $1}')] $(stat -c 'ino=%i size=%s blocks=%b' "$f" 2>/dev/null)"
				if [ -x /usr/sbin/filefrag ] || command -v filefrag >/dev/null 2>&1; then
					filefrag -v "$f" 2>/dev/null | sed -n '4,8p' |
						while read -r l; do log "$1_BADFRAG $f $l"; done
				fi
				# Does the kernel object when the same bytes are
				# re-read directly, bypassing nothing?  If the
				# read is clean twice, no checksum is being
				# consulted for this range at all.
				dd if="$f" of=/dev/null bs=4096 status=none 2>/dev/null &&
					log "$1_BADINFO $f reread=clean" ||
					log "$1_BADINFO $f reread=EIO"
				# A second digest.  If two reads of the same
				# file in the same boot disagree, the value is
				# being reconstructed differently each time,
				# which is a different finding from a stable
				# wrong value.
				log "$1_BADINFO $f second_read=$(md5sum "$f" 2>/dev/null | awk '{print $1}')"
				# How much of the file is zeros, and where?  The
				# head reads as zeros while the tail is intact,
				# so the interesting number is which SECTORS are
				# zeroed -- a whole 4K sector points somewhere
				# very different from a partial one.
				zmd=$(head -c 4096 /dev/zero | md5sum | awk '{print $1}')
				zsec=""
				nsec=$(( ( $(stat -c %s "$f" 2>/dev/null || echo 0) + 4095 ) / 4096 ))
				for k in $(seq 0 $((nsec - 1))); do
					m1=$(dd if="$f" bs=4096 skip=$k count=1 status=none 2>/dev/null |
					     md5sum | awk '{print $1}')
					[ "$m1" = "$zmd" ] && zsec="$zsec $k"
				done
				log "$1_BADZERO $f sectors=$nsec all_zero_sectors=[${zsec:- none}]"
				# Does an extent still COVER the zeroed sector?
				# A sector that reads as zeros with no error is
				# what a hole reads as -- legitimately, with no
				# checksum consulted.  Dump the whole extent map
				# rather than just the first extent, and the
				# csum map, so the two can be lined up.
				filefrag -v "$f" 2>/dev/null | sed -n '3,12p' |
					while read -r l; do log "$1_BADMAP $f $l"; done
				# The decisive one: is this range checksummed
				# at all?
				[ -x $T/umltest/csummap ] &&
					$T/umltest/csummap "$f" 2>&1 |
					while read -r l; do log "$1_BADCSUM $l"; done
			fi
		fi
	done < $T/umltest/manifest.$TAG
	# The extent map of every tracked file, once per boot.  If a file reads
	# back with content that MATCHES a checksum and yet is not what was
	# written, the metadata must be describing different data -- and then
	# its extent address will differ between the boot that reads it right
	# and the boot that reads it wrong.  That is a statement about
	# metadata, not about the RAID5 data, and it is cheap to check.
	while read -r f m sz srcmd; do
		blk=$(filefrag -v "$f" 2>/dev/null |
			awk '/^[ ]*0:/ {gsub(/\.\./,"",$4); print $4; exit}')
		log "$1_EXTENT $f logical=$((${blk:-0} * 4096)) size=$(stat -c %s "$f" 2>/dev/null) head=$(od -An -tx1 -N16 "$f" 2>/dev/null | tr -d ' \n') tail=$(dd if="$f" bs=1 skip=$(( ${sz:-4096} - 16 )) count=16 status=none 2>/dev/null | od -An -tx1 | tr -d ' \n')"
	done < $T/umltest/manifest.$TAG
	log "$1_MANIFEST total=$total bad=$bad silent=$silent manifest_wrong=$srcok"
	for g in /sys/fs/btrfs/*/raid56_write_profile; do
		[ -f $g ] && log "$1_RECOVER $(grep -E 'recover_|delivered_|repair_csum' $g | tr '\n' ' ')"
	done
	# The per-sector trace, when btrfs.raid56_trace_reads=1 is on the kernel
	# command line.  One line per data sector a RAID5/6 read returned, from
	# three points: the first read, any repair, and the hand-back.  Printed
	# at KERN_INFO, which "quiet" keeps off the console, so read the ring
	# buffer instead.
	dmesg | grep -o 'RTRACE .*' | while read -r l; do log "$1_RTRACE $l"; done
	# Whose sectors are being returned unchecked?  Take the addresses the
	# kernel just named and ask the extent tree who owns them.
	log "$1_UNVERIFIED_ADDRS $(dmesg | grep -o 'UNVERIFIED_REBUILD logical [0-9]*' | awk '{print $3}' | sort -un | tr '\n' ' ')"
	dmesg | grep -o "UNVERIFIED_REBUILD logical [0-9]*" | awk '{print $3}' |
		sort -un | head -4 | while read -r lg; do
		own=$(btrfs inspect-internal logical-resolve -P "$lg" $MNT 2>&1 | head -3 | tr '\n' ' ')
		log "$1_UNVERIFIED_OWNER logical=$lg -> ${own:-<unresolved>}"
	done
	# CONTROL for the above.  A "nothing owns this" answer from a degraded
	# read-only mount is worth nothing unless resolving a KNOWN extent on
	# the same mount works.  Take a file that exists and resolve its own
	# first extent; if that fails too, the tool cannot answer here and the
	# unowned verdicts above mean nothing.
	for probe in $MNT/old $MNT/bg1-1; do
		[ -f "$probe" ] || continue
		blk=$(filefrag -v "$probe" 2>/dev/null |
			awk '/^[ ]*0:/ {gsub(/\.\./,"",$4); print $4; exit}')
		[ -n "$blk" ] || continue
		lg=$((blk * 4096))
		own=$(btrfs inspect-internal logical-resolve -P "$lg" $MNT 2>&1 | head -2 | tr '\n' ' ')
		log "$1_RESOLVE_CONTROL $probe logical=$lg -> ${own:-<unresolved>}"
	done
	[ "$bad" = 0 ] || kmsg "csum|error|corrupt" 5
}
verify_nocow() {
	# nodatacow has no checksum: compare with the content read after the
	# recovery mount, a stale parity would give different content.
	[ -f $T/umltest/nocow.md5.$TAG ] || return 0
	local M=$(md5sum $MNT/nocow 2>&1 | awk '{print $1}')
	if [ "$M" = "$(cat $T/umltest/nocow.md5.$TAG)" ]; then log "$1_NOCOW_OK"; else
		log "$1_NOCOW_FAIL md5=$M"; kmsg "csum|error|corrupt" 5; fi
}
verify_old() {
	echo 3 > /proc/sys/vm/drop_caches
	if [ -f $T/umltest/old.md5.$TAG ]; then
		local M=$(md5sum $MNT/old 2>&1 | awk '{print $1}')
		if [ "$M" = "$(cat $T/umltest/old.md5.$TAG)" ]; then log "$1_READ_OK"; else
			log "$1_READ_FAIL md5=$M"; kmsg "csum|error|corrupt" 5; fi
	fi
	verify_manifest $1
	verify_nocow $1
}
writers_start() {
	for w in 1 2 3; do
		(
		i=0
		while [ ! -f $T/umltest/stop.$TAG ]; do
			f=$MNT/bg$w-$i
			sz=$(( (RANDOM % 15 + 1) ))
			if [ "$WRITER" = "syncfs" ]; then
				head -c $((sz * 4096)) /dev/urandom > $f
				sync -f $f
			else
				# Record BOTH the digest of the bytes handed to
				# dd and the digest of reading the file back.
				#
				# They can differ, and that is the point.  The
				# read-back happens while dm-flakey is already
				# erroring a device, so it can be served by a
				# reconstruction from parity -- and if that
				# reconstruction is wrong, the manifest records
				# an expectation that was never written.  A
				# later mismatch against the read-back digest
				# but a match against the source digest means
				# the test was wrong, not the filesystem.
				src=/tmp/src.$w.$i
				head -c $((sz * 4096)) /dev/urandom > $src
				srcmd=$(md5sum $src | awk '{print $1}')
				dd if=$src of=$f bs=4096 conv=fsync status=none \
					&& echo "$f $(md5sum $f | awk '{print $1}') $((sz * 4096)) $srcmd" \
						>> $T/umltest/manifest.$TAG
				rm -f $src
			fi
			i=$((i+1))
		done
		) &
	done
}
writers_stop() { touch $T/umltest/stop.$TAG; wait; rm -f $T/umltest/stop.$TAG; }
rm -f $T/umltest/manifest.$TAG.new
# The kernel refuses "chattr +C" on a RAID5/6 filesystem, and copies rather
# than overwriting in place for inodes that were already marked.  Scenarios
# that exist to show what happens WITHOUT that protection have to turn it off
# explicitly; it is a debug-build knob and every other boot leaves it alone.
# nocow_refuse deliberately does not call this: it tests the refusal.
allow_nodatacow() {
	echo 1 > /sys/module/btrfs/parameters/raid56_allow_nodatacow 2>/dev/null \
		|| log "NODATACOW_KNOB_FAIL"
}

deny_nodatacow() {
	echo 0 > /sys/module/btrfs/parameters/raid56_allow_nodatacow 2>/dev/null \
		|| log "NODATACOW_KNOB_FAIL"
}

# device-mapper helpers: dm_setup creates d0..dN-1 over the ubd devices
dm_setup() {
	local i=0
	DMDEVS=""
	for d in $DEVS; do
		local sz=$(blockdev --getsz $d)
		dmsetup create d$i --table "0 $sz linear $d 0" || log "DM_CREATE_FAIL $d"
		DMDEVS="$DMDEVS /dev/mapper/d$i"
		i=$((i+1))
	done
	# No udev in the guest: create the /dev/mapper nodes ourselves.
	dmsetup mknodes
	ls /dev/mapper/d0 >/dev/null || log "DM_NODE_MISSING"
}
# Register only the dm paths with btrfs so that the mount uses them.
dm_scan() { btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $DMDEVS >/dev/null 2>&1; }
dm_reload() {	# name table...
	local n=$1; shift
	dmsetup suspend --nolockfs --noflush $n && dmsetup reload $n --table "$*" && dmsetup resume $n || log "DM_RELOAD_FAIL $n"
}
dm_detach() {	# index: every IO fails, like a pulled drive
	local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
	dm_reload d$1 "0 $(blockdev --getsz $d) error"
}
dm_error_writes() {	# index: reads work, every write fails
	local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
	dm_reload d$1 "0 $(blockdev --getsz $d) flakey $d 0 0 1000 1 error_writes"
}
dm_error_reads() {	# index: writes work, every read fails
	local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
	dm_reload d$1 "0 $(blockdev --getsz $d) flakey $d 0 0 1000 1 error_reads"
}
dm_heal() {
	local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
	dm_reload d$1 "0 $(blockdev --getsz $d) linear $d 0"
}

# The 4K blocks of $MNT/nocow that the in-place overwrite targets, at a 64K
# stride so they land in different vertical stripes and on different devices.
# Overridable so a scenario that needs MORE ambiguous full stripes than the
# evidence ring has slots can ask for them, rather than having the ring's
# overflow behaviour be untestable because the workload never overflows it.
NOCOW_BLOCKS=${NOCOW_BLOCKS:-32}
NOCOW_STRIDE=${NOCOW_STRIDE:-16}          # in 4K blocks, so 64 KiB apart
NOCOW_SIZE_MB=${NOCOW_SIZE_MB:-2}
# One RAID5 full stripe is (NDEV - 1) x BTRFS_STRIPE_LEN, i.e. 192 KiB on a
# 4-device array.  Derived rather than fixed: a scenario that needs more
# devices to keep the metadata writable would otherwise walk the wrong stride
# and silently test nothing.
NOCOW_FS_BLOCKS=$(( (${NDEV:-4} - 1) * 16 ))
NOCOW_NEXT_COL=16        # +64 KiB: the next data column of the same stripe
# nocow_two_stale: full stripes walked, and the width of each overwrite in
# 4K blocks.  One column short of the full stripe, so the write is still a
# read-modify-write, while covering as many columns as possible -- the defect
# needs two of them to land on the pair of devices that are failing.
TWO_STRIPES=8
TWO_WIDTH=$(( (${NDEV:-4} - 2) * 16 ))
nocow_bad() {
	# How many of the overwritten blocks do NOT read back as the value the
	# overwrite was acknowledged to have written.  Zero means the data is
	# intact by whatever path this mount had to use to get it.
	#
	# Count what IS 'B', not what is not: a read that fails returns no bytes
	# at all, and "nothing that isn't B" used to score that as intact.
	#
	# Only an ACKNOWLEDGED overwrite has to read back as 'B'.  A refused one
	# (the kernel refuses a write it cannot make safe, see rmw_repair_first())
	# promised nothing, so either the old 'A' or the new 'B' is right -- but
	# not anything else.  nocow.acked.$TAG lists the acknowledged ones; with
	# no list every overwrite counts as acknowledged, as before.
	local bad=0 i off got list=""
	[ -f $T/umltest/nocow.acked.$TAG ] &&
		list=" $(tr '\n' ' ' < $T/umltest/nocow.acked.$TAG) "
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		off=$((i * NOCOW_STRIDE))
		got=$(dd if=$MNT/nocow bs=4096 skip=$off count=1 status=none 2>/dev/null |
		      tr -cd 'B' | wc -c)
		[ "$got" = 4096 ] && continue
		if [ -n "$list" ]; then
			case "$list" in *" $i "*) ;; *)
				got=$(dd if=$MNT/nocow bs=4096 skip=$off count=1 status=none 2>/dev/null |
				      tr -cd 'A' | wc -c)
				[ "$got" = 4096 ] && continue;;
			esac
		fi
		bad=$((bad+1))
	done
	echo $bad
}

# unprovable_*: one 1 MiB file of 'A', with some 4K blocks overwritten by
# RANDOM data while a device fails writes.
#
# The overwrite must NOT be a second uniform pattern.  A sector destroyed by a
# wrong reconstruction holds (victim ^ old ^ new), and the victim's true value
# IS the old value -- both are the original fill -- so with two patterns that
# reduces to exactly the overwrite pattern, and destroyed blocks are
# indistinguishable from legitimately overwritten ones.  Random @new makes the
# damage random, so any block that was never overwritten and is no longer
# uniformly 'A' has been written over with something nobody stored.
#
# The overwrites must be CONTIGUOUS runs one column wide, not a stride.  A
# stride puts a stale sector in the same vertical stripe of every column, and
# those are precisely the blocks the scan then skips -- so the stale sectors and
# the checked sectors never share a vertical stripe and the reconstruction is
# always correct.  A 64 KiB run covers one whole column (or parts of two), which
# leaves at least one other column untouched at exactly those verticals.  Those
# untouched blocks are the victims: reconstructing them consumes the stale
# column of the same row.
UNPROV_BLOCKS=256
UNPROV_RUN=16            # 64 KiB: one column
UNPROV_PERIOD=48         # (NDEV-1) x 16: one full stripe on a 4-device array
# In the checksummed arm the victim is a separate file that the test never
# writes twice, so no block of it is ever "deliberately overwritten".
unprov_overwritten() {
	[ "${CSUMVICTIM:-0}" = 1 ] && return 1
	[ $(( $1 % UNPROV_PERIOD )) -lt $UNPROV_RUN ]
}
unprov_scan() {	# echoes "<garbage> <unreadable>"
	local garbage=0 unread=0 i n na
	for i in $(seq 0 $((UNPROV_BLOCKS-1))); do
		# Skip the blocks the test itself overwrote.
		unprov_overwritten $i && continue
		n=$(dd if=$MNT/victim bs=4096 skip=$i count=1 status=none 2>/dev/null | wc -c)
		if [ "$n" != 4096 ]; then unread=$((unread+1)); continue; fi
		na=$(dd if=$MNT/victim bs=4096 skip=$i count=1 status=none 2>/dev/null |
		     tr -d 'A' | wc -c)
		[ "$na" = 0 ] || garbage=$((garbage+1))
	done
	echo "$garbage $unread"
}

case "$MODE" in
prepare)
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS $MNTDEV
	log "mount options: $(grep umltest /proc/mounts | head -1)"
	stats
	# Committed data: 128K written and committed before the injected write.
	dd if=/dev/urandom of=$MNT/old bs=128K count=1 status=none
	sync
	md5sum $MNT/old | awk '{print $1}' > $T/umltest/old.md5.$TAG
	log "old md5 $(cat $T/umltest/old.md5.$TAG)"
	log "old layout: $(filefrag -v $MNT/old | sed -n 4p | tr -s ' ')"
	if [ -n "$CONVERT" ]; then
		writers_start
		btrfs balance start -f -dconvert=${CONVERT%%:*} -mconvert=${CONVERT##*:} $MNT >/dev/null 2>&1 \
			&& log "CONVERT_OK $CONVERT" || log "CONVERT_FAIL $CONVERT"
		writers_stop
		sync
		log "old layout after convert: $(filefrag -v $MNT/old | sed -n 4p | tr -s ' ')"
		stats
	fi
	# Arm the injection: the next sub-stripe RMW drops P/Q (1) or data (2)
	# writes, waits for the rest to land and panics.
	echo $CRASH > /sys/module/btrfs/parameters/raid56_crash_point 2>/dev/null || log "CRASH_ARM_FAIL"
	dd if=/dev/urandom of=$MNT/new bs=4K count=1 status=none
	sync
	# Not reached when the injection fired.
	log "NO_CRASH: raid56_crash_point=$(cat /sys/module/btrfs/parameters/raid56_crash_point)"
	umount $MNT
	finish
	;;
recover)
	do_mount $OPTS $MNTDEV
	kmsg "write-intent|regenerat|crash injection|tree-log" 8
	stats
	[ -f $MNT/nocow ] && md5sum $MNT/nocow | awk '{print $1}' > $T/umltest/nocow.md5.$TAG
	verify_old FULL
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
ro_replay)
	# Read-only mount with a dirty tree log: the replay writes, so the
	# recovery and the log must run before it, even read-only.
	do_mount $OPTS,ro $MNTDEV
	kmsg "write-intent|tree-log|crash injection" 8
	stats "ro sysfs"
	[ -f $MNT/nocow ] && md5sum $MNT/nocow | awk '{print $1}' > $T/umltest/nocow.md5.$TAG
	verify_old RO
	umount $MNT || log "UMOUNT_FAIL"
	do_mount $OPTS $MNTDEV
	kmsg "write-intent" 4
	stats "rw sysfs"
	verify_old FULL
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
prepare_fsync)
	# Nothing committed in the crashed full stripe: everything in it is
	# only referenced from the tree log (fsync without a transaction
	# commit), the log tree blocks and the fsync'ed data.  f shares its
	# vertical stripe with the crashed write of 'new'.
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS $MNTDEV
	stats
	MAN=$T/umltest/manifest.$TAG
	: > $MAN
	dd if=/dev/urandom of=$MNT/f bs=4K count=1 conv=fsync status=none
	echo "$MNT/f $(md5sum $MNT/f | awk '{print $1}')" >> $MAN
	dd if=/dev/urandom of=$MNT/g bs=4K count=15 conv=fsync status=none
	echo "$MNT/g $(md5sum $MNT/g | awk '{print $1}')" >> $MAN
	log "f layout: $(filefrag -v $MNT/f | sed -n 4p | tr -s ' ')"
	log "g layout: $(filefrag -v $MNT/g | sed -n 4p | tr -s ' ')"
	echo $CRASH > /sys/module/btrfs/parameters/raid56_crash_point 2>/dev/null || log "CRASH_ARM_FAIL"
	dd if=/dev/urandom of=$MNT/new bs=4K count=1 conv=fsync status=none
	log "NO_CRASH: raid56_crash_point=$(cat /sys/module/btrfs/parameters/raid56_crash_point)"
	umount $MNT
	finish
	;;
inplace)
	# A nodatacow file overwritten in place with full stripe writes: the
	# overwritten sectors are referenced, the crash leaves them with a
	# stale parity unless the write was recorded.
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS $MNTDEV
	stats
	allow_nodatacow
	touch $MNT/nocow; chattr +C $MNT/nocow
	dd if=/dev/urandom of=$MNT/nocow bs=64K count=12 conv=fsync status=none
	sync
	log "nocow layout: $(filefrag -v $MNT/nocow | sed -n 4p | tr -s ' ')"
	echo $CRASH > /sys/module/btrfs/parameters/raid56_crash_point 2>/dev/null || log "CRASH_ARM_FAIL"
	dd if=/dev/urandom of=$MNT/nocow bs=64K count=12 conv=fsync,notrunc status=none
	log "NO_CRASH: raid56_crash_point=$(cat /sys/module/btrfs/parameters/raid56_crash_point)"
	umount $MNT
	finish
	;;
remount)
	# Read-only mount must not touch the disks; the remount to read-write
	# must run the recovery before anything else is written.
	do_mount $OPTS,ro $MNTDEV
	stats "ro sysfs"
	kmsg "write-intent" 3
	mount -o remount,rw $MNT || { log "REMOUNT_RW_FAIL"; finish; }
	kmsg "write-intent|regenerat" 4
	stats "rw sysfs"
	verify_old FULL
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
degraded)
	# DEGRADED_MOUNT=ro makes this pass OBSERVE instead of mutate.
	#
	# A degraded read-write mount runs the write-intent log's recovery and
	# can write: it repairs, it regenerates parity, and on RAID5/6 with a
	# device missing it does that from reconstructions.  With one such boot
	# per omitted device, the second is no longer looking at the array the
	# first one saw, and a mismatch against the original manifest stops
	# meaning "the filesystem lost this" and starts meaning "the filesystem
	# lost this, or an earlier degraded mount rewrote it".  nocow_probe
	# already mounts ro for exactly this reason.
	do_mount ${DEGRADED_MOUNT:-$OPTS},degraded $MNTDEV
	kmsg "write-intent log:" 3
	verify_old DEGRADED
	ls $MNT/new >/dev/null 2>&1 && log "new exists (unexpected, was not committed)"
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
compat)
	# Kernel without write-intent support: RW mount must be refused, RO ok.
	if mount -o $OPTS $MNTDEV $MNT 2>/dev/null; then log "OLDKERNEL_RW_MOUNT_ALLOWED"; umount $MNT; else
		log "OLDKERNEL_RW_MOUNT_REFUSED"; kmsg "compat|unsupported|read-only" 3; fi
	if mount -o $OPTS,ro $MNTDEV $MNT 2>/dev/null; then log "OLDKERNEL_RO_MOUNT_OK"; verify_old OLDKERNEL_RO; umount $MNT; else
		log "OLDKERNEL_RO_MOUNT_FAIL"; fi
	finish
	;;
check)
	btrfs check $MNTDEV > $T/umltest/check.$TAG 2>&1 && log "CHECK_OK" || log "CHECK_FAIL rc=$?"
	grep -E "error|ERROR|found|compat" $T/umltest/check.$TAG | head -n 5 | while read -r l; do log "check: $l"; done
	btrfs inspect-internal dump-super $MNTDEV | grep -E "compat_ro_flags|incompat_flags" | while read -r l; do log "super: $l"; done
	python3 $T/umltest/raid56_wib_dump.py $DEVS 2>/dev/null | head -n 12 |
		while read -r l; do log "wib: $l"; done || log "WIB_DUMP_FAIL"
	finish
	;;
stress)
	# Concurrent small-file writers with fsync; the host kills the UML
	# process at a random moment.  Every file whose fsync returned is
	# recorded (name + md5) in a host-side manifest before the next write.
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS $MNTDEV
	MAN=$T/umltest/manifest.$TAG
	: > $MAN
	# A nodatacow file overwritten in place by one writer: its sectors are
	# updated in place (torn content after a crash is expected for it).
	allow_nodatacow
	touch $MNT/nocow; chattr +C $MNT/nocow; dd if=/dev/zero of=$MNT/nocow bs=4096 count=64 conv=fsync status=none
	for w in 1 2 3 4; do
		(
		i=0
		while true; do
			f=$MNT/w$w-$i
			sz=$(( (RANDOM % 15 + 1) ))
			dd if=/dev/urandom of=$f bs=4096 count=$sz conv=fsync status=none
			m=$(md5sum $f | awk '{print $1}')
			echo "$f $m" >> $MAN
			i=$((i+1))
			dd if=/dev/urandom of=$MNT/nocow bs=4096 count=1 seek=$((RANDOM % 64)) conv=notrunc,fsync status=none
		done
		) &
	done
	echo "STRESS_START"
	sleep 600
	;;
verify)
	do_mount "$OPTS${OMITTED:+,degraded}" $MNTDEV
	echo 3 > /proc/sys/vm/drop_caches
	MAN=$T/umltest/manifest.$TAG
	total=0; bad=0
	while read -r f m; do
		total=$((total+1))
		got=$(md5sum $f 2>/dev/null | awk '{print $1}')
		if [ "$got" != "$m" ]; then bad=$((bad+1)); log "BAD $f expected $m got $got"; fi
	done < $MAN
	cat $MNT/nocow > /dev/null 2>&1 && log "nocow readable" || log "NOCOW_READ_FAIL"
	log "VERIFY total=$total bad=$bad omitted=${OMITTED:-none}"
	kmsg "write-intent log:" 3
	stats
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
replace)
	# Device replace, remove and add while sub-stripe writes are in
	# flight: exercises the dev-replace finishing path that waits for
	# in-flight bios under device_list_mutex, and the RCU device list
	# traversal of the log writer (lockdep is enabled in this kernel).
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF /dev/ubda /dev/ubdb /dev/ubdc /dev/ubdd || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS /dev/ubda
	watchdog 150
	writers_start
	sleep 3
	btrfs replace start -B -f 1 /dev/ubde $MNT && log "REPLACE_OK" || log "REPLACE_FAIL"
	sleep 2
	btrfs device add -f /dev/ubda $MNT && log "ADD_OK" || log "ADD_FAIL"
	sleep 2
	btrfs device remove /dev/ubdb $MNT && log "REMOVE_OK" || log "REMOVE_FAIL"
	sleep 2
	writers_stop
	sync
	stats
	umount $MNT || log "UMOUNT_FAIL"
	btrfs check /dev/ubda 2>&1 | grep -E "error|found|ERROR" | head -n 3 | while read -r l; do log "check: $l"; done
	finish
	;;
convert)
	# No RAID56 at mkfs time: the log must be enabled when the first
	# RAID5 chunk is created by the balance, at the next commit.
	mkfs.btrfs -K -q -f -d raid1 -m raid1 $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS $MNTDEV
	stats before
	writers_start
	sleep 2
	btrfs balance start -f -dconvert=raid5 -mconvert=raid5 $MNT >/dev/null 2>&1 && log "CONVERT_OK" || log "CONVERT_FAIL"
	sleep 2
	writers_stop
	sync
	stats after
	umount $MNT || log "UMOUNT_FAIL"
	btrfs inspect-internal dump-super $MNTDEV | grep -E "compat_ro_flags" | while read -r l; do log "super: $l"; done
	finish
	;;
convert_away)
	# RAID5 -> RAID1 with writers: the log stays enabled (flag set), the
	# remaining RAID5 chunks are still protected until they are gone.
	mkfs.btrfs -K -q -f -d raid5 -m raid5 $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS $MNTDEV
	dd if=/dev/urandom of=$MNT/old bs=128K count=1 status=none; sync
	md5sum $MNT/old | awk '{print $1}' > $T/umltest/old.md5.$TAG
	writers_start
	sleep 2
	btrfs balance start -f -dconvert=raid1 -mconvert=raid1 $MNT >/dev/null 2>&1 && log "CONVERT_AWAY_OK" || log "CONVERT_AWAY_FAIL"
	writers_stop
	sync
	stats after
	verify_old FULL
	umount $MNT || log "UMOUNT_FAIL"
	btrfs inspect-internal dump-super $MNTDEV | grep -E "compat_ro_flags" | while read -r l; do log "super: $l"; done
	finish
	;;
toggle)
	# Feature toggling through sysfs while writes are in flight.
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS,noraid56_write_intent $MNTDEV
	watchdog 150
	writers_start
	sleep 2
	for i in 1 2 3; do
		echo 1 > /sys/fs/btrfs/*/features/raid56_write_intent || log "ENABLE_FAIL"
		sleep 1
		echo 0 > /sys/fs/btrfs/*/features/raid56_write_intent || log "DISABLE_FAIL"
		sleep 1
	done
	echo 1 > /sys/fs/btrfs/*/features/raid56_write_intent
	writers_stop
	sync
	stats
	umount $MNT || log "UMOUNT_FAIL"
	btrfs inspect-internal dump-super $MNTDEV | grep -E "compat_ro_flags" | while read -r l; do log "super: $l"; done
	finish
	;;
detach)
	# A drive that fails every IO in the middle of writes (pulled cable),
	# no crash.  Its stripes are kept in the log (sticky); after the drive
	# is back, the next mount regenerates their parity.  Then the array
	# must survive losing any other device.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	log "fs devices: $(btrfs filesystem show $MNT 2>/dev/null | grep devid | tr -s ' ' | tr '\n' ';')"
	dd if=/dev/urandom of=$MNT/old bs=128K count=1 status=none; sync
	md5sum $MNT/old | awk '{print $1}' > $T/umltest/old.md5.$TAG
	writers_start
	sleep 2
	dm_detach $FAIL; log "detached device $FAIL"
	sleep 4
	writers_stop
	sync
	stats "while detached"
	kmsg "write-intent|lost|error" 4
	dm_heal $FAIL; log "healed device $FAIL"
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all
	finish
	;;
nocow_stale)
	# Does the mount-time recovery DESTROY nodatacow data that was still
	# recoverable before it ran?
	#
	# A nodatacow sector has no checksum.  scrub_verify_one_sector() has no
	# choice but to trust it ("For cases without csum, we have no other
	# choice but to trust it"), so it clears the sector's error bit, and
	# scrub_raid56_parity_stripe() then recomputes the parity from it.
	# When that sector is the one a failed write left STALE, the parity was
	# the only place the acknowledged content still existed -- and the
	# recovery overwrites it with a parity computed from the stale content.
	#
	# This boot builds that state with a real device write error, not a
	# crash: the write must be ACKNOWLEDGED (one fault is inside RAID5
	# tolerance) and the stripe recorded, which is exactly what makes the
	# next mount scrub it.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount "$OPTS${NOLOG:+,noraid56_write_intent}" /dev/mapper/d0
	log "log state: $(cat /sys/fs/btrfs/*/raid56_write_intent 2>/dev/null | tr '\n' ' ')"
	allow_nodatacow
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	lsattr $MNT/nocow 2>/dev/null | grep -q C || log "NOT_NODATACOW"
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	log "nocow layout: $(filefrag -v $MNT/nocow 2>/dev/null | sed -n 4p | tr -s ' ')"
	stats "before"
	# Every write to this device now fails; reads still work.  A sub-stripe
	# write whose data sector lands here takes one fault, which RAID5
	# tolerates, so the write is acknowledged and the stripe is recorded.
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	acked=0; failed=0; rm -f $T/umltest/nocow.acked.$TAG
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		if dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		   dd of=$MNT/nocow bs=4096 seek=$((i * NOCOW_STRIDE)) count=1 \
		      conv=notrunc,fsync status=none 2>/dev/null; then
			acked=$((acked+1)); echo $i >> $T/umltest/nocow.acked.$TAG
		else
			failed=$((failed+1))
		fi
	done
	sync
	log "in-place overwrites: $acked acknowledged, $failed refused"
	stats "after write errors"
	kmsg "write-intent|raid56" 6
	# Heal it so the next boots see a complete, healthy array: the point is
	# what recovery does, not what a broken device does.
	dm_heal $FAIL; log "healed device $FAIL"
	sync
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all
	finish
	;;
pausehang_log_prep)
	# Leave BOTH a dirty tree log and recorded stripes, then die without
	# unmounting.  The next mount then runs btrfs_wib_recover_after_replay(),
	# which is the one recovery path that runs after the transaction kthread
	# has been started -- the only place a commit can overlap it.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	dd if=/dev/urandom of=$MNT/base bs=1M count=8 conv=fsync status=none
	sync
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	for i in $(seq 0 39); do
		dd if=/dev/urandom of=$MNT/base bs=4096 count=1 seek=$((i * 48)) \
		   conv=notrunc,fsync status=none 2>/dev/null
	done
	# fsync without sync: the data is referenced only by the tree log, so
	# the next mount has a log to replay before it can finish recovering.
	for i in $(seq 0 7); do
		dd if=/dev/urandom of=$MNT/logged$i bs=64K count=1 conv=fsync \
		   status=none 2>/dev/null
	done
	stats "recorded with a dirty log"
	log "PREP_DONE"
	# No umount: leave the log dirty.
	# Power off, NOT reboot: sysrq-b restarts the guest, which re-runs this
	# same init and repeats the whole preparation until the host timeout
	# kills it.  No sync first -- the dirty tree log is the point.
	echo o > /proc/sysrq-trigger
	sleep 60
	;;
pausehang_log)
	# Mount normally.  btrfs_wib_recover_after_replay() runs from
	# open_ctree() AFTER cleaner_kthread and transaction_kthread have
	# started and after the tree log has been replayed, so unlike
	# btrfs_remount_rw() a commit really can land while it is running.
	echo ${DELAY:-6000} > /sys/module/btrfs/parameters/raid56_recovery_delay_ms \
		2>/dev/null || log "DELAY_ARM_FAIL"
	log "recovery delay armed: $(cat /sys/module/btrfs/parameters/raid56_recovery_delay_ms 2>/dev/null)"
	watchdog 200
	log "MOUNT_START"
	if timeout 240 mount -o rw,commit=1 $MNTDEV $MNT; then
		log "MOUNT_DONE"
	else
		log "MOUNT_STUCK rc=$?"
		echo w > /proc/sysrq-trigger 2>/dev/null; sleep 3
	fi
	echo 0 > /sys/module/btrfs/parameters/raid56_recovery_delay_ms 2>/dev/null
	log "pause samples: $(dmesg | grep -c "recovery delay: pause_req")"
	log "samples with a pauser: $(dmesg | grep "recovery delay: pause_req" | grep -vc "pause_req 0")"
	dmesg | grep "recovery delay: pause_req" | grep -v "pause_req 0" | tail -4 | while read -r l; do log "PAUSER: $l"; done
	stats "after mount"
	kmsg "blocked for more|replay|write-intent" 8
	umount $MNT 2>/dev/null
	finish
	;;
pausehang_prep)
	# Leave a filesystem with recorded stripes and unmount it, so the next
	# boot's btrfs_wib_load() puts them in wib->pending -- which is the only
	# thing btrfs_wib_recover() iterates.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	dd if=/dev/urandom of=$MNT/base bs=1M count=8 conv=fsync status=none
	sync
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	for i in $(seq 0 39); do
		dd if=/dev/urandom of=$MNT/base bs=4096 count=1 seek=$((i * 48)) \
		   conv=notrunc,fsync status=none 2>/dev/null
	done
	sync
	dm_heal $FAIL
	stats "recorded"
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
pausehang)
	# Can the write-intent log's recovery wedge against the scrub pause
	# protocol?
	#
	#   btrfs_scrub_pause():  inc pause_req; wait until paused == running
	#   scrub_pause_off():    wait until pause_req == 0; then dec paused
	#
	# The recovery is deliberately absent from scrubs_running, but
	# scrub_raid56_parity_stripe() calls scrub_blocked_if_needed(), which
	# increments scrubs_paused.  While the recovery sits there paused is 1
	# and running is 0, so a commit waits for 1 == 0 while the recovery
	# waits for pause_req to reach 0 -- which that commit is holding above
	# zero.
	#
	# Mount READ-ONLY, so btrfs_wib_load() fills wib->pending and
	# btrfs_wib_rw_mount() is deferred; the remount below is then the thing
	# that actually runs the recovery.  Mounting read-write first would
	# consume wib->pending at mount time and leave the remount nothing to
	# do, which is what an earlier version of this scenario did.
	do_mount ro $MNTDEV
	stats "mounted ro"
	pend=$(grep -o "pending_recovery_regions [0-9]*" /sys/fs/btrfs/*/raid56_write_intent 2>/dev/null | awk '{print $2}' | head -1)
	log "pending regions before remount: ${pend:-unknown}"
	[ "${pend:-0}" = 0 ] && log "NOTHING_TO_RECOVER"
	echo ${DELAY:-40} > /sys/module/btrfs/parameters/raid56_recovery_delay_ms \
		2>/dev/null || log "DELAY_ARM_FAIL"
	log "recovery delay armed: $(cat /sys/module/btrfs/parameters/raid56_recovery_delay_ms 2>/dev/null)"
	watchdog 150
	log "REMOUNT_RW_START"
	if timeout 180 mount -o remount,rw,commit=1 $MNT; then
		log "REMOUNT_RW_DONE"
	else
		log "REMOUNT_RW_STUCK rc=$?"
		echo w > /proc/sysrq-trigger 2>/dev/null
		sleep 3
	fi
	echo 0 > /sys/module/btrfs/parameters/raid56_recovery_delay_ms 2>/dev/null
	stats "after remount"
	log "pause samples: $(dmesg | grep -c "recovery delay: pause_req")"
	log "samples with a pauser: $(dmesg | grep "recovery delay: pause_req" | grep -vc "pause_req 0")"
	dmesg | grep "recovery delay: pause_req" | tail -4 | while read -r l; do log "sample: $l"; done
	kmsg "blocked for more|scrub|write-intent" 8
	umount $MNT 2>/dev/null
	finish
	;;
nocow_refuse)
	# Is the unsafe combination refused, and is the refusal the thing that
	# happens rather than a silent substitution?
	#
	# check_fsflags_compatible() rejects FS_NOCOW_FL on a filesystem with
	# RAID5/6 chunks, the same way it already rejects it on zoned.  The
	# caller is told, instead of getting copy-on-write while believing they
	# asked for something else.
	#
	# The fallback still has to exist for states a user can no longer
	# create: an inode marked NODATACOW before any RAID5/6 chunk existed,
	# or whose extents a balance moved onto one afterwards.  Both are built
	# here.
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS $MNTDEV
	log "incompat raid56 set: $(btrfs inspect-internal dump-super $MNTDEV 2>/dev/null | grep -o 'RAID56' | head -1)"
	touch $MNT/direct
	if chattr +C $MNT/direct 2>$MNT/.err; then
		log "REFUSE_FAIL: chattr +C was accepted on a RAID5/6 filesystem"
		lsattr $MNT/direct 2>/dev/null | while read -r l; do log "  attrs: $l"; done
	else
		log "REFUSED_OK: $(cat $MNT/.err 2>/dev/null | tail -1)"
	fi
	rm -f $MNT/.err
	# And the legacy shape: a directory marked +C would normally propagate
	# the flag to new files.  On a RAID5/6 filesystem that must be refused
	# too, or the flag arrives by the back door.
	mkdir -p $MNT/cdir
	if chattr +C $MNT/cdir 2>/dev/null; then
		log "REFUSE_FAIL_DIR: chattr +C accepted on a directory"
	else
		log "REFUSED_DIR_OK"
	fi
	stats "after refusal"
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
nocow_cow)
	# Does forcing copy-on-write actually remove the in-place
	# read-modify-write on RAID5/6?
	#
	# A NODATACOW file is created and overwritten exactly as the nocow_rmw
	# scenario does, with a device failing every write.  If the force is
	# working, can_nocow_file_extent() refuses every one of those extents,
	# the overwrites are copied to fresh space instead, and
	# inplace_full_stripe_writes stays at zero -- there is no in-place RMW
	# left for a stale sector to arise in.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	lsattr $MNT/nocow 2>/dev/null | grep -q C || log "NOT_NODATACOW"
	# Back on for the writes.  An inode carrying +C that the kernel would
	# no longer let you set is exactly the state the copy-on-write fallback
	# exists for: marked before any RAID5/6 chunk existed, or moved onto
	# one by a balance.
	deny_nodatacow
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	log "first extent: $(filefrag -v $MNT/nocow 2>/dev/null | sed -n 4p | tr -s ' ')"
	stats "after create"
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	acked=0; rm -f $T/umltest/nocow.acked.$TAG
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		dd of=$MNT/nocow bs=4096 seek=$((i * NOCOW_STRIDE)) count=1 \
		   conv=notrunc,fsync status=none 2>/dev/null &&
			{ acked=$((acked+1)); echo $i >> $T/umltest/nocow.acked.$TAG; }
	done
	sync
	log "overwrites: $acked of $NOCOW_BLOCKS acknowledged"
	log "after overwrite: $(filefrag -v $MNT/nocow 2>/dev/null | sed -n 4p | tr -s ' ')"
	log "extent count now: $(filefrag $MNT/nocow 2>/dev/null | grep -o '[0-9]* extent' | head -1)"
	dm_heal $FAIL
	stats "after overwrite"
	# Read it back with the failing device omitted is a separate boot; here
	# just record whether any in-place full stripe write happened at all.
	kmsg "raid56|write-intent" 4
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
nocow_rmw)
	# Does an ordinary FAULT-FREE write destroy data a previous failed
	# write left recoverable?
	#
	# To compute a new parity, a sub-stripe write must read the data
	# stripes it is not writing.  A nodatacow sector has no checksum, so a
	# sector left stale by an earlier failed write is believed, and the
	# parity -- which held the acknowledged value -- is recomputed from it.
	# No crash, no second device failure, no scrub: the very next write to
	# the same full stripe is enough.
	#
	# Pass 1 writes column 0 of each full stripe while a device fails
	# writes.  The device is then healed, and pass 2 writes column 1 of the
	# SAME full stripes with every write succeeding.  Reading pass 1's
	# blocks with the failed device omitted says whether the parity still
	# has them.
	#
	# A RAID5 full stripe here is 3 x 64K, so +64K is the next column.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	lsattr $MNT/nocow 2>/dev/null | grep -q C || log "NOT_NODATACOW"
	# Big enough for every block the passes write: at NOCOW_FS_BLOCKS apart,
	# 32 of them span 6 MiB, and a write past the end is not an in-place
	# overwrite at all -- it allocates, packs the new extents into shared
	# full stripes, and tests something else.
	dd if=/dev/zero bs=1M count=$(( NOCOW_BLOCKS * NOCOW_FS_BLOCKS * 4096 / 1048576 + 2 )) \
		status=none | tr '\000' 'A' > $MNT/nocow
	# One pre-made block instead of a tr(1) pipeline per write: the loop
	# below issues one write per data column per stripe, and under UML those
	# pipelines cost more than the scenario does.
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > /tmp/bblock
	sync
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	acked=0; rm -f $T/umltest/nocow.acked.$TAG
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		dd of=$MNT/nocow bs=4096 seek=$((i * NOCOW_FS_BLOCKS)) count=1 \
		   conv=notrunc,fsync status=none 2>/dev/null &&
			{ acked=$((acked+1)); echo $i >> $T/umltest/nocow.acked.$TAG; }
	done
	sync
	log "pass1 (device failing): $acked of $NOCOW_BLOCKS acknowledged"
	stats "after pass1"
	# Healthy again: pass 2 takes no faults at all.
	dm_heal $FAIL; log "healed device $FAIL"
	# Evict the stripe cache.  Pass 1's writes were ACKNOWLEDGED, so
	# rmw_rbio() left RBIO_CACHE_READY_BIT set and the cache still holds the
	# content the caller was told is on disk -- correct content, which pass 2
	# would then use, and nothing would be destroyed.  That is real but it is
	# not durable: the cache holds RBIO_CACHE_SIZE (1024) rbios and is only
	# cleared at unmount, so any busy filesystem cycles through it.  Touch
	# more distinct full stripes than it can hold, so pass 2 has to read the
	# disk like it would on a real array.
	allow_nodatacow
	touch $MNT/churn; chattr +C $MNT/churn 2>/dev/null
	fallocate -l 256M $MNT/churn 2>/dev/null || log "FALLOCATE_FAIL"
	n=0
	for i in $(seq 0 1200); do
		dd if=/dev/zero of=$MNT/churn bs=4096 count=1 \
		   seek=$((i * NOCOW_FS_BLOCKS)) conv=notrunc status=none 2>/dev/null \
			&& n=$((n+1))
	done
	sync
	log "stripe-cache churn: $n distinct full stripes touched (cache holds 1024)"
	clean=0
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'C' |
		dd of=$MNT/nocow bs=4096 seek=$((i * NOCOW_FS_BLOCKS + NOCOW_NEXT_COL)) count=1 \
		   conv=notrunc,fsync status=none 2>/dev/null && clean=$((clean+1))
	done
	sync
	log "pass2 (healthy, same full stripes): $clean of $NOCOW_BLOCKS acknowledged"
	stats "after pass2"
	kmsg "raid56|write-intent" 6
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all
	finish
	;;
nocow_rmw_probe)
	# Read pass 1's blocks with the failing device omitted, so every one of
	# them must come from the parity.
	# A measurement of what the parity holds, not a user's read: see
	# nocow_probe for why the undecidable rebuild is let through.
	echo 1 > /sys/module/btrfs/parameters/raid56_read_trusts_ambiguous 2>/dev/null
	do_mount ro,degraded $MNTDEV
	# Same rules as nocow_bad(): an acknowledged block must read back as
	# 'B' (all 4096 bytes -- a failed read has none), a refused one as its
	# old 'A' or its new 'B'.
	bad=0
	list=" $(tr '\n' ' ' < $T/umltest/nocow.acked.$TAG 2>/dev/null) "
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		got=$(dd if=$MNT/nocow bs=4096 skip=$((i * NOCOW_FS_BLOCKS)) count=1 \
		      status=none 2>/dev/null | tr -cd 'B' | wc -c)
		[ "$got" = 4096 ] && continue
		case "$list" in *" $i "*) ;; *)
			got=$(dd if=$MNT/nocow bs=4096 skip=$((i * NOCOW_FS_BLOCKS)) count=1 \
			      status=none 2>/dev/null | tr -cd 'A' | wc -c)
			[ "$got" = 4096 ] && continue;;
		esac
		bad=$((bad+1))
	done
	log "NOCOW_RMW bad=$bad of $NOCOW_BLOCKS"
	echo $bad > $T/umltest/nocow.rmw.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
nocow_replay_prep)
	# Leave error records AND a dirty tree log, then die without
	# unmounting.  The next read-write mount replays the log and then runs
	# btrfs_wib_recover_after_replay(), which is the recovery entry point
	# that handles error records exclusively -- and the one nocow_stale.sh
	# cannot reach, because it unmounts cleanly and so leaves no log.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	lsattr $MNT/nocow 2>/dev/null | grep -q C || log "NOT_NODATACOW"
	dd if=/dev/zero bs=1M count=$NOCOW_SIZE_MB status=none | tr '\000' 'A' > $MNT/nocow
	sync
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	acked=0; rm -f $T/umltest/nocow.acked.$TAG
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		dd of=$MNT/nocow bs=4096 seek=$((i * NOCOW_STRIDE)) count=1 \
		   conv=notrunc,fsync status=none 2>/dev/null &&
			{ acked=$((acked+1)); echo $i >> $T/umltest/nocow.acked.$TAG; }
	done
	log "in-place overwrites: $acked of $NOCOW_BLOCKS acknowledged"
	dm_heal $FAIL; log "healed device $FAIL"
	# fsync-only files so a tree log is left dirty; no sync, no umount.
	for i in $(seq 0 5); do
		dd if=/dev/urandom of=$MNT/logged$i bs=64K count=1 conv=fsync \
		   status=none 2>/dev/null
	done
	stats "recorded with a dirty log"
	# Power off, NOT reboot: sysrq-b restarts the guest, which re-runs this
	# same init and repeats the whole preparation until the host timeout
	# kills it.  No sync first -- the dirty tree log is the point.
	echo o > /proc/sysrq-trigger
	sleep 60
	;;
nocow_replay_probe)
	# nologreplay keeps this mount from replaying the log, which is what
	# would otherwise drag btrfs_wib_rw_mount() in even on a read-only
	# mount -- so this really does read the array without any recovery
	# having run.  The failed device is omitted, so every one of the
	# overwritten blocks has to come from the parity.
	# Plain ro,degraded.  The log has already been replayed by the recovery
	# boot, so open_ctree() has no reason to call btrfs_wib_rw_mount() here
	# and no recovery runs.  (ro,nologreplay,degraded is refused outright at
	# option-parsing time -- "bad option", with no message from btrfs.)
	# A measurement of what the parity holds, not a user's read: see
	# nocow_probe for why the undecidable rebuild is let through.
	echo 1 > /sys/module/btrfs/parameters/raid56_read_trusts_ambiguous 2>/dev/null
	do_mount ro,degraded $MNTDEV
	bad=$(nocow_bad)
	log "NOCOW_REPLAY_${PROBE:-x} bad=$bad of $NOCOW_BLOCKS"
	echo $bad > $T/umltest/nocow.replay.${PROBE:-x}.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
nocow_replay_recover)
	# Replays the log, then recovers the error records.
	do_mount $OPTS $MNTDEV
	stats "after replay recovery"
	kmsg "replay|write-intent" 6
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
nocow_two_stale)
	# Does forcing a rebuild for every sector the log calls stale make
	# reads WORSE than not having the record at all?
	#
	# The state-machine model says it does, in the states where the
	# rebuild does not fit inside the profile's tolerance
	# (tools/testing/btrfs/scrub_policy_model.py --all-readers, reader
	# "stale-any": 6835 of 14446 RAID5 states).  This builds one of them.
	#
	# TWO devices fail their writes, and each overwrite spans two data
	# columns of one full stripe.  When both of those columns land on the
	# failing pair the write takes two faults, which RAID5 does not
	# tolerate, so it is REFUSED -- the caller is told nothing landed and
	# the committed content is still what was there before.  But the log
	# records both columns stale, and a reader that acts on that record
	# without checking the budget asks for two reconstructions from one
	# parity.  Upstream would have returned the committed content off the
	# disk.
	#
	# So: reads that fail, or that come back as neither the old content
	# nor the new, are the defect.  Either value is acceptable -- which one
	# depends on whether that stripe's write was refused or acknowledged,
	# and the point is that neither is garbage.
	#
	# The legacy arm has hung twice with nothing on the console after
	# mkfs; dump the blocked tasks well before the host gives up on it.
	watchdog ${WATCH:-600}
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	[ "${LEGACY:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_stale_read_legacy \
			2>/dev/null || log "LEGACY_ARM_FAIL"
		log "legacy stale-read behaviour armed"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	lsattr $MNT/nocow 2>/dev/null | grep -q C || log "NOT_NODATACOW"
	dd if=/dev/zero bs=1M count=16 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	log "layout: $(filefrag -v $MNT/nocow 2>/dev/null | sed -n 4p | tr -s ' ')"
	# Two devices, so a write spanning two data columns of one full stripe
	# can take two faults at once.
	dm_error_writes 1; dm_error_writes 2
	log "write errors on devices 1 and 2"
	# One 4K write per data column, each its own read-modify-write.  Each
	# takes at most ONE fault -- the column either sits on a failing device
	# or it does not -- so every write stays inside RAID5 tolerance and is
	# acknowledged, and the filesystem stays writable.  Two failing devices
	# then leave TWO columns of the same full stripe recorded stale, which
	# is one more than a single parity can rebuild.  A write wide enough to
	# take both faults at once is refused instead, and the transaction
	# abort that follows ends the scenario before it has built anything.
	#
	# Deliberately no conv=fsync either: that syncs the tree log on every
	# write, and with two devices erroring that aborts the transaction too.
	# Its own block: /tmp/bblock is left behind by another scenario, and
	# without it every overwrite here fails silently.
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > /tmp/bblock.two
	for i in $(seq 0 $((TWO_STRIPES-1))); do
		for c in $(seq 0 $((NDEV-2))); do
			dd if=/tmp/bblock.two of=$MNT/nocow bs=4096 \
			   seek=$((i * NOCOW_FS_BLOCKS + c * NOCOW_NEXT_COL)) \
			   count=1 conv=notrunc status=none 2>/dev/null
		done
	done
	sync
	log "per-column overwrites: $TWO_STRIPES stripes x $((NDEV-1)) columns"
	stats "after write errors"
	dm_heal 1; dm_heal 2; log "healed devices 1 and 2"
	# From the disks: the folios stayed uptodate after the failed
	# writeback, and a read served from them never reaches the code under
	# test -- both arms then read every block back as written.
	echo 3 > /proc/sys/vm/drop_caches
	# Read back one block from each of the two columns of every stripe
	# touched, with every device present and healthy.
	garbage=0; ioerr=0; olds=0; news=0
	for i in $(seq 0 $((TWO_STRIPES-1))); do
		for c in $(seq 0 $((NDEV-2))); do
			off=$((i * NOCOW_FS_BLOCKS + c * NOCOW_NEXT_COL))
			if ! out=$(dd if=$MNT/nocow bs=4096 skip=$off count=1 \
				      status=none 2>/dev/null); then
				ioerr=$((ioerr+1)); continue
			fi
			if [ -z "$(printf %s "$out" | tr -d 'A')" ]; then
				olds=$((olds+1))
			elif [ -z "$(printf %s "$out" | tr -d 'B')" ]; then
				news=$((news+1))
			else
				garbage=$((garbage+1))
			fi
		done
	done
	log "NOCOW_TWO_STALE garbage=$garbage ioerr=$ioerr old=$olds new=$news of $((TWO_STRIPES*(NDEV-1)))"
	echo "$garbage $ioerr" > $T/umltest/nocow.twostale.${LEGACY:-0}.$TAG
	kmsg "raid56|write-intent" 6
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
nocow_scrub)
	# The same question asked of plain "btrfs scrub", with the write-intent
	# log switched off entirely (noraid56_write_intent), so nothing in this
	# series is involved.  scrub_stripe() passes regen_parity=true
	# unconditionally, and scrub_verify_one_sector() is upstream code, so
	# the expectation is that upstream scrub destroys the parity copy just
	# as the recovery does.
	do_mount $OPTS,noraid56_write_intent $MNTDEV
	log "log state: $(cat /sys/fs/btrfs/*/raid56_write_intent 2>/dev/null | tr '\n' ' ')"
	btrfs scrub start -B $MNT 2>&1 | while read -r l; do log "scrub: $l"; done
	log "NOCOW_DIRECT bad=$(nocow_bad) of $NOCOW_BLOCKS"
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
nocow_persist_prep)
	# Build the state, then unmount CLEANLY, so everything the next mount
	# knows comes off the disk.  That is the whole point: the stale record
	# used to live only in memory, so it did not survive this boundary and
	# the scrub after the next mount had nothing to consult.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	[ "${NOPERSIST:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_stale_no_persist \
			2>/dev/null || log "NOPERSIST_ARM_FAIL"
		log "stale record will NOT be persisted (control)"
	}
	# Record every parity as not describing the data, so a stripe that also
	# has a named stale column has nothing left to be rebuilt from.  That
	# is the ambiguous case, and the repair path must decline it.
	[ "${FAKEBADPAR:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_stale_fake_bad_parity \
			2>/dev/null || log "FAKEBADPAR_ARM_FAIL"
		log "every parity will be recorded as unusable (ambiguous case)"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	lsattr $MNT/nocow 2>/dev/null | grep -q C || log "NOT_NODATACOW"
	dd if=/dev/zero bs=1M count=${PREP_SIZE_MB:-2} status=none | tr '\000' 'A' > $MNT/nocow
	sync
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	acked=0; rm -f $T/umltest/nocow.acked.$TAG
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		dd of=$MNT/nocow bs=4096 seek=$((i * NOCOW_STRIDE)) count=1 \
		   conv=notrunc,fsync status=none 2>/dev/null &&
			{ acked=$((acked+1)); echo $i >> $T/umltest/nocow.acked.$TAG; }
	done
	sync
	log "in-place overwrites: $acked of $NOCOW_BLOCKS acknowledged"
	dm_heal $FAIL; log "healed device $FAIL"
	# Give a repair the kernel queued on the failed writes time to find the
	# device healthy again (rmw_repair.sh, the trigger arms).
	[ "${SETTLE:-0}" -gt 0 ] && sleep $SETTLE
	stats "after write errors"
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
nocow_persist_scrub)
	# A fresh mount, then a plain user scrub.  Mount-time recovery runs
	# first and leaves the stripe recorded; the question is whether the
	# scrub that follows still knows which SIDE of the stripe is wrong.
	dm_setup; dm_scan
	do_mount $OPTS /dev/mapper/d0
	stats "after recovery"
	kmsg "write-intent" 6
	# What a recovery helper would see, before anything repairs it: the
	# preserved half of the contract, read through the ioctl rather than
	# inferred from a counter.
	if [ -x $T/umltest/wibdump ]; then
		# No block group start passed, so wibdump withholds the
		# per-stripe verdict rather than grouping columns that are not
		# in the same stripe.  What is asserted here is the raw record:
		# how much the kernel can NAME versus how much it cannot.
		$T/umltest/wibdump $MNT $((NDEV-1)) 1 2>&1 |
			grep -E 'WIBDUMP|REPAIRABLE|AMBIGUOUS' |
			while read -r l; do log "wibdump-before: $l"; done
	else
		log "WIBDUMP_MISSING"
	fi
	if [ "${EVIDENCE:-0}" != 0 ] && [ -x $T/umltest/evidence ]; then
		# EVIDENCE picks what to do with the channel while the scrub
		# runs.  1 is the real workflow; the others exist because the
		# paths they reach had never been executed by anything.
		#
		#   1  drain continuously (the design: see below)
		#   2  arm and deliberately do NOT drain, so the four-slot ring
		#      fills and dropped_full has to count the rest
		#   3  same, with btrfs.raid56_evidence_max_cols lowered on the
		#      kernel command line so every stripe is "too wide" and
		#      dropped_wide counts instead
		#   4  hammer arm/read/disarm against the captures
		#   5  arm, never drain, and disarm with entries still
		#      queued -- the one loss the kernel cannot undo, so it
		#      has to say so.  Arm 4 cannot reach it: its helper
		#      reads immediately before every disarm, so the ring is
		#      empty by the time it lets go.  Also asks for
		#      DISARM_IF_EMPTY first, which must refuse.
		#   6  the real workflow, in one process: "evidence collect"
		#      arms bound to itself, drains while the scrub runs, and
		#      disarms only once nothing is left
		#   7  a bound helper that never reads is killed mid-scrub
		#      with evidence queued: the kernel must disarm when its
		#      file closes, say what that discarded, and the scrub
		#      must still finish
		EVDIR=$T/umltest/evdir.$TAG
		rm -rf $EVDIR; mkdir -p $EVDIR
		# 6 and 7 arm bound to their own process; the others arm here
		# with a short-lived one, which is exactly why they stay unbound.
		case "${EVIDENCE}" in
		6)
			$T/umltest/evidence $MNT collect $EVDIR > /tmp/collect.out 2>&1 &
			cpid=$!
			for _ in $(seq 1 50); do
				grep -q EVIDENCE_COLLECTING /tmp/collect.out 2>/dev/null && break
				sleep 0.1
			done
			;;
		7)
			$T/umltest/evidence $MNT hold > /tmp/hold.out 2>&1 &
			hpid=$!
			for _ in $(seq 1 50); do
				grep -q EVIDENCE_HOLDING /tmp/hold.out 2>/dev/null && break
				sleep 0.1
			done
			log "evidence: $(cat /tmp/hold.out)"
			;;
		*)
			$T/umltest/evidence $MNT arm 2>&1 |
				while read -r l; do log "evidence: $l"; done
			;;
		esac
		btrfs scrub start -B $MNT > /tmp/scrub.out 2>&1 &
		spid=$!
		case "${EVIDENCE}" in
		7)
			# Kill it while the ring is full and captures are waiting
			# on it, which is the harder moment: a capture asleep in
			# its wait has to notice the channel vanished and give up,
			# not sleep on.  Fall back to killing after the scrub if
			# the ring never fills.
			for _ in $(seq 1 300); do
				q=$($T/umltest/evidence $MNT stats 2>/dev/null |
					sed -n 's/.*queued=\([0-9]*\).*/\1/p')
				[ "${q:-0}" -ge 4 ] && break
				kill -0 $spid 2>/dev/null || break
				sleep 0.1
			done
			log "evidence: queued before kill: ${q:-?}"
			kill -9 $hpid 2>/dev/null
			wait $hpid 2>/dev/null
			log "evidence: killed the bound reader"
			;;
		esac
		case "${EVIDENCE}" in
		1)
			# Stream the evidence out WHILE the scrub runs.
			# Draining after it finishes would be waiting for a
			# four-slot ring to have overflowed, which is the
			# failure this design exists to avoid.
			while kill -0 $spid 2>/dev/null; do
				n=$($T/umltest/evidence $MNT drain $EVDIR 2>&1 |
					tee /tmp/ev.out | grep -c 'EVIDENCE stripe')
				grep -E 'EVIDENCE stripe' /tmp/ev.out |
					while read -r l; do log "evidence: $l"; done
				# Only pause when there was nothing to take.  A
				# scrub declines stripes in bursts, so sleeping
				# after a productive drain is how a four-slot
				# ring overflows while a helper is right there.
				[ "${n:-0}" = 0 ] && sleep 1
			done
			;;
		4)
			# No drain: arm, read and disarm as fast as the ioctl
			# allows, for as long as the scrub runs.  Nothing is
			# asserted about what comes back -- the assertion is
			# that the scrub finishes at all.
			$T/umltest/evidence $MNT race 25 2>&1 |
				while read -r l; do log "evidence: $l"; done
			# The race leaves it disarmed; arm again so the
			# counters below have somewhere to come from.
			$T/umltest/evidence $MNT arm >/dev/null 2>&1
			;;
		esac
		wait $spid
		log "evidence: scrub finished"
		case "${EVIDENCE}" in
		6)
			# The scrub is over; tell collect to finish.  It does the
			# last drain and DISARM_IF_EMPTY itself.
			kill -TERM $cpid 2>/dev/null
			wait $cpid
			log "evidence: collect exited rc=$?"
			grep -E 'EVIDENCE' /tmp/collect.out |
				while read -r l; do log "evidence: $l"; done
			;;
		7)
			# The channel should be gone.  A READ that still works
			# means the kernel never noticed the reader die.
			$T/umltest/evidence $MNT stats 2>&1 |
				while read -r l; do log "evidence: after kill: $l"; done
			;;
		*)
			# Before draining anything: what did the kernel have to
			# throw away?  Draining first would empty the ring and
			# make a full ring indistinguishable from an idle one.
			$T/umltest/evidence $MNT stats 2>&1 |
				while read -r l; do log "evidence: $l"; done
			# stats asks for zero bytes, so every queued entry comes
			# back -ERANGE and stays queued: it reports without
			# consuming.
			[ "${EVIDENCE}" = 5 ] ||
				$T/umltest/evidence $MNT drain $EVDIR 2>&1 |
				grep -E 'EVIDENCE' | while read -r l; do log "evidence: $l"; done
			# With entries queued, DISARM_IF_EMPTY must refuse and
			# leave everything readable.
			[ "${EVIDENCE}" = 5 ] &&
				$T/umltest/evidence $MNT disarm-if-empty 2>&1 |
				while read -r l; do log "evidence: $l"; done
			$T/umltest/evidence $MNT disarm 2>&1 |
				while read -r l; do log "evidence: $l"; done
			;;
		esac
		# The kernel says when a disarm threw queued stripes away, and
		# when a bound reader went away.  KERN_WARNING/INFO, which
		# "quiet" keeps off the console, and the dmesg dump further down
		# filters for scrub messages, which these are not -- so take them
		# out of the ring buffer here or they are invisible to the test
		# that exists to check them.
		dmesg | grep -oE "evidence channel disarmed with.*|evidence channel's reader closed it.*|evidence channel armed.*" |
			while read -r l; do log "evidence: $l"; done
		log "EVIDENCE_FILES=$(ls $EVDIR/*.data 2>/dev/null | wc -l) EVIDENCE_BYTES=$(cat $EVDIR/*.data 2>/dev/null | wc -c)"
		while read -r l; do log "scrub: $l"; done < /tmp/scrub.out
	else
		btrfs scrub start -B $MNT 2>&1 |
			while read -r l; do log "scrub: $l"; done
	fi
	stats "after scrub"
	kmsg "scrub|write-intent" 6
	# Did the scrub REPAIR, or merely decline to make things worse?  A
	# record that survives the scrub is a stripe whose redundancy was never
	# restored, and it will never be restored: nothing else retires one.
	sticky_after=$(sed -n 's/.*sticky_blocks \([0-9]*\).*/\1/p' \
		/sys/fs/btrfs/*/raid56_write_intent 2>/dev/null | head -1)
	log "NOCOW_STICKY_AFTER_SCRUB $sticky_after"
	if [ -x $T/umltest/wibdump ]; then
		$T/umltest/wibdump $MNT $((NDEV-1)) 1 2>&1 | grep -E 'WIBDUMP' |
			while read -r l; do log "wibdump-after: $l"; done
	fi
	echo "${sticky_after:-?}" > $T/umltest/nocow.sticky.$TAG
	log "NOCOW_DIRECT bad=$(nocow_bad) of $NOCOW_BLOCKS"
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
unprovable_prep)
	# Leave one device's columns stale, then unmount cleanly.  Same shape as
	# nocow_persist_prep, but the file is big enough to span several full
	# stripes so that a later read error on a DIFFERENT device is likely to
	# land in a vertical stripe that has a stale sector in it.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	# The caller mounts with noraid56_write_intent.  raid56_stale_no_persist
	# is NOT enough here: it zeroes the stale field but @sticky is still
	# recorded, so the next mount's recovery still scrubs those stripes and
	# recomputes the parity from the data -- the two agree again and every
	# later reconstruction is CORRECT, which is the right outcome and tests
	# nothing.  With the log off there is no record and no recovery, which
	# is also the configuration where this guard matters most: unchecksummed
	# data, a parity that does not describe it, and nothing that knows.
	if [ "${CSUMVICTIM:-0}" = 1 ]; then
		# The victim is CHECKSUMMED, so it cannot be the file that gets
		# overwritten in place -- an overwrite of it would be copy-on-
		# write and would create no divergence at all.  Use a separate
		# nodatacow "bait" for that, and interleave the two a column at
		# a time so they land in DIFFERENT COLUMNS OF THE SAME full
		# stripes.  Sync between chunks so the allocator lays them down
		# in the order written rather than batching delalloc.
		touch $MNT/bait; chattr +C $MNT/bait || { log "CHATTR_FAIL"; finish; }
		lsattr $MNT/bait 2>/dev/null | grep -q C || log "NOT_NODATACOW"
		for c in $(seq 0 $((UNPROV_BLOCKS / UNPROV_RUN - 1))); do
			dd if=/dev/zero bs=4096 count=$UNPROV_RUN status=none |
			tr '\000' 'A' |
			dd of=$MNT/victim bs=4096 seek=$((c * UNPROV_RUN)) \
			   conv=notrunc status=none 2>/dev/null
			sync
			dd if=/dev/zero bs=4096 count=$UNPROV_RUN status=none |
			tr '\000' 'C' |
			dd of=$MNT/bait bs=4096 seek=$((c * UNPROV_RUN)) \
			   conv=notrunc status=none 2>/dev/null
			sync
		done
		lsattr $MNT/victim 2>/dev/null | grep -q C && log "VICTIM_IS_NODATACOW"
		dm_error_writes $FAIL; log "write errors on device $FAIL"
		acked=0; tried=0
		for i in $(seq 0 $((UNPROV_BLOCKS-1))); do
			tried=$((tried+1))
			dd if=/dev/urandom bs=4096 count=1 status=none |
			dd of=$MNT/bait bs=4096 seek=$i count=1 \
			   conv=notrunc,fsync status=none 2>/dev/null && acked=$((acked+1))
		done
	else
		touch $MNT/victim; chattr +C $MNT/victim || { log "CHATTR_FAIL"; finish; }
		lsattr $MNT/victim 2>/dev/null | grep -q C || log "NOT_NODATACOW"
		dd if=/dev/zero bs=4096 count=$UNPROV_BLOCKS status=none |
			tr '\000' 'A' > $MNT/victim
		sync
		dm_error_writes $FAIL; log "write errors on device $FAIL"
		acked=0; tried=0
		# One column-wide run per full stripe, so every vertical of that
		# column is overwritten while the other columns of the same rows
		# are left as written.  Whichever run lands on device $FAIL
		# becomes a stale column, and the untouched columns beside it
		# become the victims.
		for i in $(seq 0 $((UNPROV_BLOCKS-1))); do
			unprov_overwritten $i || continue
			tried=$((tried+1))
			dd if=/dev/urandom bs=4096 count=1 status=none |
			dd of=$MNT/victim bs=4096 seek=$i count=1 \
			   conv=notrunc,fsync status=none 2>/dev/null && acked=$((acked+1))
		done
	fi
	sync
	log "overwrites acknowledged: $acked of $tried"
	dm_heal $FAIL
	stats "after write errors"
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
unprovable_scrub)
	# The destroyer.  Device $VICTIM is healthy and holds good data, but its
	# reads now fail.  Scrub reconstructs those sectors from the parity --
	# which, for any vertical stripe that also has a stale sector on $FAIL,
	# yields a value nothing ever committed.  The sectors carry no checksum,
	# so scrub_verify_one_sector() "has no other choice but to trust it",
	# the error bit is cleared, and the guess is written back over the good
	# data.  With the guard in place it must decline instead.
	dm_setup; dm_scan
	do_mount $OPTS /dev/mapper/d0
	stats "after recovery"
	[ "${TRUSTREBUILD:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_scrub_trusts_rebuild \
			2>/dev/null || log "TRUSTREBUILD_ARM_FAIL"
		log "scrub will trust an unverifiable rebuild (control)"
	}
	dm_error_reads $VICTIM; log "read errors on device $VICTIM"
	btrfs scrub start -B $MNT 2>&1 | while read -r l; do log "scrub: $l"; done
	dm_heal $VICTIM
	stats "after scrub"
	kmsg "not written back|left untouched|unrepaired" 8
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
unprovable_diag)
	# Precondition check, asserted by nothing: mount degraded with $VICTIM
	# omitted, so every one of its sectors MUST come from the parity.  If
	# the parity diverged from the data, that reconstruction is wrong and
	# these blocks come back as something nobody wrote -- which is exactly
	# what the scrub would then persist.  A zero here means the scenario
	# never built the state, and the scrub arms below cannot mean anything.
	dm_setup; dm_scan
	# A measurement of what the parity holds, not a user's read: see
	# nocow_probe for why the undecidable rebuild is let through.
	echo 1 > /sys/module/btrfs/parameters/raid56_read_trusts_ambiguous 2>/dev/null
	do_mount ro,degraded /dev/mapper/d0
	set -- $(unprov_scan)
	log "UNPROV_DIAG_GARBAGE=$1 UNPROV_DIAG_UNREAD=$2 (device $VICTIM omitted)"
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
unprovable_probe)
	# Every device healthy, so this reads what is actually ON the platters.
	# The file has no checksum, so nothing filters what comes back.
	dm_setup; dm_scan
	do_mount ro /dev/mapper/d0
	set -- $(unprov_scan)
	log "UNPROV_GARBAGE=$1 UNPROV_UNREAD=$2 of $UNPROV_BLOCKS"
	echo "$1" > $T/umltest/unprov.garbage.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
nocow_probe)
	# Read the overwritten blocks WITHOUT letting recovery run: read-only,
	# so btrfs_wib_rw_mount() is skipped.  The host omits the device whose
	# sectors the failed writes left stale, which forces every one of them
	# to be reconstructed from the parity -- so this measures what the
	# parity still holds.
	#
	# A measurement, not a user's read: where the record leaves the rebuild
	# undecidable the kernel now refuses it (EIO) rather than vouch for
	# it, which is right for a user and hides from this probe exactly the
	# parity content it exists to see.  Let the rebuild through here.
	echo 1 > /sys/module/btrfs/parameters/raid56_read_trusts_ambiguous 2>/dev/null
	do_mount ro,degraded $MNTDEV
	bad=$(nocow_bad)
	log "NOCOW_PROBE_${PROBE:-x} bad=$bad of $NOCOW_BLOCKS"
	echo $bad > $T/umltest/nocow.bad.${PROBE:-x}.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
repair_freeze)
	# The repair queued on a failed write must not write to a frozen
	# filesystem.  Fault some writes, heal the device, freeze at once, and
	# count the writes the block layer completed on every device while
	# frozen -- then thaw and see the repair happen.  CONTROL=1 restores a
	# repair that ignores the freeze.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_repair_ignores_freeze ||
			log "CONTROL_ARM_FAIL"
		log "repair ignores freeze (control)"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	dm_error_writes $FAIL
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		dd of=$MNT/nocow bs=4096 seek=$((i * NOCOW_STRIDE)) count=1 \
		   conv=notrunc,fsync status=none 2>/dev/null
	done
	dm_heal $FAIL
	fsfreeze -f $MNT || log "FREEZE_FAIL"
	dm_writes() { local n=0 d; for d in /sys/block/dm-*/stat; do
		n=$((n + $(awk '{print $5}' $d))); done; echo $n; }
	rok() { sed -n 's/.*repair_ok \([0-9]*\).*/\1/p' /sys/fs/btrfs/*/raid56_write_intent | head -1; }
	w0=$(dm_writes); ok0=$(rok)
	sleep ${FROZEN_FOR:-20}
	w1=$(dm_writes); ok1=$(rok)
	fsfreeze -u $MNT || log "THAW_FAIL"
	sleep ${FROZEN_FOR:-20}
	ok2=$(rok)
	stats "after thaw"
	log "REPAIR_FREEZE writes_frozen=$((w1 - w0)) repairs_frozen=$((ok1 - ok0)) repairs_after=$((ok2 - ok1))"
	echo "$((w1 - w0)) $((ok1 - ok0)) $((ok2 - ok1))" > $T/umltest/repair.freeze.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
rmw_torn)
	# A crash in the write that repairs a stale column.  'B' is written
	# while its column's device fails writes (stale, named); then 'C' goes
	# into the next column of the same row with raid56_crash_point=2 armed:
	# that write's data does not land, its parity does, and the kernel
	# panics.  If the write-back of 'B' went out in the same batch, it is
	# dropped with the data, and the record still names the column against
	# a parity that has just changed.  CONTROL=1 restores that single batch.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	echo 600000 > /sys/module/btrfs/parameters/raid56_repair_delay_ms
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_rmw_single_phase ||
			log "CONTROL_ARM_FAIL"
		log "single-phase repair (control)"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	L=$(python3 $T/umltest/raid56_layout.py $MNT/nocow /dev/mapper/d0 2>&1)
	log "layout: $L"
	echo "$L" > $T/umltest/layout.$TAG
	eval "$L"
	[ -n "${FO_B:-}" ] || { log "LAYOUT_FAIL"; finish; }
	dm_error_writes $IDX_B
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		dd of=$MNT/nocow bs=4096 seek=$((FO_B / 4096)) count=1 conv=notrunc,fsync \
		status=none 2>/dev/null && log "B acknowledged" || log "B refused"
	dm_heal $IDX_B
	sync
	echo 2 > /sys/module/btrfs/parameters/raid56_crash_point
	log "crash armed"
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'C' |
		dd of=$MNT/nocow bs=4096 seek=$((FO_C / 4096)) count=1 conv=notrunc,fsync \
		status=none 2>/dev/null
	log "NO_CRASH"
	finish
	;;
rmw_torn_verify)
	# After the crash: a normal read-write mount (the recovery runs), then
	# is 'B' still what reads back, and what is on its platter?
	eval "$(cat $T/umltest/layout.$TAG)"
	do_mount $OPTS /dev/ubda
	kmsg "write-intent|scrub:" 6
	echo 3 > /proc/sys/vm/drop_caches
	got=$(dd if=$MNT/nocow bs=4096 skip=$((FO_B / 4096)) count=1 status=none 2>/dev/null |
	      tr -cd 'B' | wc -c)
	umount $MNT || log "UMOUNT_FAIL"
	dev=/dev/ubd$(echo abcdefgh | cut -c$((IDX_B + 1)))
	plat=$(dd if=$dev bs=4096 skip=$((PHYS_B / 4096)) count=1 status=none 2>/dev/null |
	       tr -cd 'B' | wc -c)
	log "RMW_TORN b_read=$got b_platter=$plat"
	echo "$got $plat" > $T/umltest/rmw.torn.$TAG
	finish
	;;
repair_pin)
	# A repair held in flight (raid56_repair_hold_ms) while its block group
	# is emptied, balanced away, and its device space refilled with new,
	# checksummed data.  Unpinned, the repair wakes and writes its stripe
	# into space that now belongs to the new data.  CONTROL=1 is that.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	HOLD=${HOLD:-40000}
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_repair_no_pin ||
			log "CONTROL_ARM_FAIL"
		log "repair not pinned (control)"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	dm_error_writes $FAIL
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		dd of=$MNT/nocow bs=4096 seek=$((i * NOCOW_STRIDE)) count=1 \
		   conv=notrunc,fsync status=none 2>/dev/null
	done
	dm_heal $FAIL
	# Only now: held during the faulted writes, every repair would sit on
	# its stripe lock with the writes queued behind it.  The retries after
	# the heal are the ones that get held.
	echo $HOLD > /sys/module/btrfs/parameters/raid56_repair_hold_ms
	held=no
	for _ in $(seq 1 60); do
		dmesg | grep -q "repair of full stripe .* holding" && { held=yes; break; }
		sleep 1
	done
	log "repair holding: $held"
	chunks() { btrfs inspect-internal dump-tree -t chunk /dev/mapper/d0 2>/dev/null |
		awk '/CHUNK_ITEM/{k=$0} /type DATA/{d=1; print "chunk", k} d&&/stripe [0-9]/{print "   ", $0} /num_stripes/{} /^\titem/{d=0}' |
		sed 's/.*CHUNK_ITEM \([0-9]*\).*/chunk \1/' | head -24; }
	chunks | while read -r l; do log "before: $l"; done
	t0=$(date +%s)
	rm -f $MNT/nocow; sync
	btrfs balance start --full-balance -d $MNT > /tmp/bal.out 2>&1
	log "balance rc=$? after $(( $(date +%s) - t0 ))s: $(tail -1 /tmp/bal.out)"
	# Fill the filesystem.  The device space the relocated chunk gave up is
	# reused by whichever new chunk the allocator happens to place there --
	# measured, only the last one, on one device -- so partial refills miss
	# it.  Full, every freed byte holds new, checksummed data.
	dd if=/dev/urandom of=$MNT/new bs=1M ${REFILL_MB:+count=$REFILL_MB} conv=fsync \
		status=none 2>/dev/null
	log "refill size $(du -m $MNT/new | cut -f1) MiB"
	sync
	want=$(md5sum $MNT/new | awk '{print $1}')
	log "refilled after $(( $(date +%s) - t0 ))s"
	chunks | while read -r l; do log "after: $l"; done
	dmesg | grep -E "holding|still recorded" | while read -r l; do log "dmesg: $l"; done
	sleep $(( HOLD / 1000 + 15 ))
	# RAID5 heals a damaged data sector on the first read -- checksum
	# failure, rebuild from parity, write back -- so the file reading back
	# intact proves nothing.  Count the corrections: checksum failures in
	# the log, and the devices' corruption counters.
	corr() { btrfs device stats $MNT 2>/dev/null |
		awk '/corruption_errs/ {n += $2} END {print n + 0}'; }
	cf0=$(dmesg | grep -c "csum failed"); ce0=$(corr)
	echo 3 > /proc/sys/vm/drop_caches
	got=$(md5sum $MNT/new 2>/dev/null | awk '{print $1}')
	[ "$got" = "$want" ] && ok=1 || ok=0
	cf=$(( $(dmesg | grep -c "csum failed") - cf0 )); ce=$(( $(corr) - ce0 ))
	pm0=$(sed -n 's/.*parity_mismatch_vertical_stripes \([0-9]*\).*/\1/p' /sys/fs/btrfs/*/raid56_write_profile | head -1)
	btrfs scrub start -B $MNT > /tmp/scrub.out 2>&1
	grep -iE "error|csum" /tmp/scrub.out | head -3 | while read -r l; do log "scrub: $l"; done
	# A stray write that lands on the new chunk's PARITY leaves the file
	# reading back fine; the scrub finds the parity not matching its data.
	pm1=$(sed -n 's/.*parity_mismatch_vertical_stripes \([0-9]*\).*/\1/p' /sys/fs/btrfs/*/raid56_write_profile | head -1)
	pm=$(( ${pm1:-0} - ${pm0:-0} ))
	kmsg "still recorded in flight|holding|raid56:" 6
	log "REPAIR_PIN held=$held new_intact=$ok csum_failed=$cf corruption_errs=$ce parity_mismatch=$pm"
	echo "$held $ok $(( cf + ce + pm ))" > $T/umltest/repair.pin.$TAG
	# The unpinned control has hung here: say where if it does again.
	watchdog 120
	umount $MNT || log "UMOUNT_FAIL"
	log "unmounted"
	dmsetup remove_all 2>/dev/null
	finish
	;;
alert)
	# Does a person find out?  Every channel btrfs has for "RAID5/6 writes
	# are in trouble" is recorded by a listener while one episode runs:
	#   1. a device fails a write the parity covers    -> degraded
	#   2. it heals and the queued repair runs         -> ok
	#   3. it fails for good: a write is refused (EIO) and the repair gives
	#      up                                          -> failing
	#   4. it heals and a scrub runs                   -> ok
	# CONTROL=1 runs the same writes with no fault: every listener must
	# stay silent and the state must stay ok.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	[ -n "$H" ] || { log "NO_HEALTH_FILE"; finish; }
	A=$T/umltest/alert.$TAG; rm -rf $A; mkdir -p $A
	python3 $T/umltest/raid56_alert_listen.py uevent $A/uevent &
	python3 $T/umltest/raid56_alert_listen.py poll $A/poll $H &
	python3 $T/umltest/raid56_alert_listen.py fanotify $A/fan $MNT &
	sleep 2
	snap() { log "HEALTH[$1] $(tr '\n' '|' < $H)"; sed -n 1p $H | cut -d' ' -f2 > $A/state.$1; }
	wr() {	# name offset char [dd flags]: one 4K block, fsync'ed
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' "$3" |
		dd of=$MNT/nocow bs=4096 seek=$(($2 / 4096)) count=1 conv=notrunc,fsync \
		   status=none $4 2>/dev/null && r=ok || r=EIO
		log "write $1: $r"; echo $r > $A/write.$1
	}
	echo 200 > /sys/module/btrfs/parameters/raid56_repair_delay_ms
	touch $MNT/nocow; chattr +C $MNT/nocow
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	L=$(python3 $T/umltest/raid56_layout.py $MNT/nocow /dev/mapper/d0 2>&1)
	log "layout: $L"
	eval "$L"
	[ -n "${FO_B:-}" ] || { log "LAYOUT_FAIL"; finish; }
	snap start
	F=$IDX_B
	[ "${CONTROL:-0}" = 1 ] && F=
	# 1: a fault the parity covers
	[ -n "$F" ] && dm_error_writes $F
	wr b1 $FO_B B
	sleep 3; snap fault
	# 2: healed, the queued repair runs
	[ -n "$F" ] && dm_heal $F
	sleep 8; snap healed
	# 3: for good.  B again goes stale; C in the next column of the same
	# row must then put B back first, cannot, and is refused -- once by
	# O_DIRECT, once buffered with no fsync (the error then only reaches a
	# later fsync, and a monitor through fanotify).
	[ -n "$F" ] && dm_error_writes $F
	wr b2 $((FO_B + 4096)) B
	wr c_direct $FO_C C oflag=direct
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'D' |
		dd of=$MNT/nocow bs=4096 seek=$(((FO_C + 4096) / 4096)) count=1 conv=notrunc \
		status=none 2>/dev/null
	sync
	python3 -c 'import os,sys; fd=os.open(sys.argv[1], os.O_WRONLY); os.fsync(fd)' \
		$MNT/nocow 2>/dev/null && r=ok || r=EIO
	log "later fsync of the buffered write: $r"; echo $r > $A/write.d_fsync
	sleep 20; snap failing
	# 4: healed and scrubbed
	[ -n "$F" ] && dm_heal $F
	btrfs scrub start -B $MNT > /tmp/scrub.out 2>&1; log "scrub rc=$?"
	sleep 5; snap recovered
	sleep 2
	kill $(jobs -p) 2>/dev/null
	# All of it: a warning does not reach a quiet console.
	dmesg | grep -a "raid56:" > $A/dmesg
	kmsg "raid56:|health" 6
	log "ALERT devid_b=$((IDX_B + 1)) uevents=$(grep -c BTRFS_RAID56_HEALTH $A/uevent) polls=$(grep -c woken $A/poll) fanotify=$(grep -c FAN_FS_ERROR $A/fan)"
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
alert_read)
	# A read whose rebuild needs a column the log records stale, because
	# another device of the same row is gone.  It used to come back WRONG
	# with no error for data without a checksum; now it must fail with EIO
	# and raise an alert.  READ_TRUST=1 restores the old behaviour (the
	# control that shows the wrong data); CONTROL=1 leaves every device in.
	[ "${READ_TRUST:-0}" = 1 ] &&
		echo 1 > /sys/module/btrfs/parameters/raid56_read_trusts_ambiguous
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	echo 600000 > /sys/module/btrfs/parameters/raid56_repair_delay_ms
	touch $MNT/nocow; chattr +C $MNT/nocow
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	L=$(python3 $T/umltest/raid56_layout.py $MNT/nocow /dev/mapper/d0 2>&1)
	log "layout: $L"
	eval "$L"
	[ -n "${IDX_C:-}" ] || { log "LAYOUT_FAIL"; finish; }
	dm_error_writes $IDX_B
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		dd of=$MNT/nocow bs=4096 seek=$((FO_B / 4096)) count=1 conv=notrunc,fsync \
		status=none 2>/dev/null && log "B acknowledged" || log "B refused"
	dm_heal $IDX_B
	sync
	# The second hole has to be KNOWN before the read -- a device missing
	# at mount.  One that only fails during the read makes the rebuild
	# fail, and the read returns EIO: visible, not silent.  ro, so mount
	# recovery does not run.
	umount $MNT || log "UMOUNT_FAIL"
	if [ "${CONTROL:-0}" = 1 ]; then
		do_mount ro /dev/mapper/d0
	else
		dmsetup remove d$IDX_C || log "REMOVE_FAIL"
		btrfs device scan --forget >/dev/null 2>&1
		for i in $(seq 0 $((NDEV-1))); do
			[ $i = $IDX_C ] || btrfs device scan /dev/mapper/d$i >/dev/null 2>&1
		done
		m=$([ $IDX_C = 0 ] && echo 1 || echo 0)
		do_mount ro,degraded /dev/mapper/d$m
	fi
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	echo 3 > /proc/sys/vm/drop_caches
	# Read the MISSING device's column (C), in the row of the stale one.
	# Its rebuild takes the stale column as it is on disk ('A' where 'B'
	# was acknowledged), so C comes back as A ^ B ^ A = 'B', not its 'A'.
	# (Reading the stale column itself is diverted as a checksum failure
	# and fails with EIO -- visible.)
	got=$(dd if=$MNT/nocow bs=4096 skip=$((FO_C / 4096)) count=1 status=none 2>/dev/null |
	      od -An -c | head -1 | tr -s ' ' | cut -c1-12)
	sleep 3
	log "read back [$got]"
	log "HEALTH[after] $(tr '\n' '|' < $H)"
	dmesg | grep -a "raid56:" > $T/umltest/alert_read.$TAG
	log "ALERT_READ unverifiable=$(sed -n 's/^read_unverifiable //p' $H) state=$(sed -n 's/^state //p' $H)"
	echo "$(sed -n 's/^read_unverifiable //p' $H) $(sed -n 's/^state //p' $H)" > $T/umltest/alert_read.result.$TAG
	umount $MNT 2>/dev/null || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
rmw_cache)
	# The stripe-cache path of a read-modify-write.  'B' is written while
	# its column's device fails writes: accepted within the tolerance, the
	# column left stale and named, and the full stripe cached with the
	# believed content.  Then 'C' goes into the next column of the same
	# vertical stripe while the PARITY device fails writes.  Served from
	# the cache, that write never reads, so it neither puts 'B' back nor
	# refuses -- and its parity write failing leaves 'B''s column and the
	# parity both wrong in the same row.  CONTROL=1 restores that.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	# Keep the repair queued on the fault out of it.
	echo 600000 > /sys/module/btrfs/parameters/raid56_repair_delay_ms
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_rmw_trust_cache ||
			log "CONTROL_ARM_FAIL"
		log "stripe cache trusted (control)"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	L=$(python3 $T/umltest/raid56_layout.py $MNT/nocow /dev/mapper/d0 2>&1)
	log "layout: $L"
	eval "$L"
	[ -n "${FO_B:-}" ] || { log "LAYOUT_FAIL"; finish; }
	dm_error_writes $IDX_B
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
		dd of=$MNT/nocow bs=4096 seek=$((FO_B / 4096)) count=1 conv=notrunc,fsync \
		status=none 2>/dev/null && log "B acknowledged" || log "B refused"
	dm_heal $IDX_B
	dm_error_writes $IDX_P
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'C' |
		dd of=$MNT/nocow bs=4096 seek=$((FO_C / 4096)) count=1 conv=notrunc,fsync \
		status=none 2>/dev/null && log "C acknowledged" || log "C refused"
	dm_heal $IDX_P
	sync
	stats "after writes"
	echo 3 > /proc/sys/vm/drop_caches
	got=$(dd if=$MNT/nocow bs=4096 skip=$((FO_B / 4096)) count=1 status=none 2>/dev/null |
	      tr -cd 'B' | wc -c)
	umount $MNT || log "UMOUNT_FAIL"
	plat=$(dd if=$DEV_B bs=4096 skip=$((PHYS_B / 4096)) count=1 status=none 2>/dev/null |
	       tr -cd 'B' | wc -c)
	log "RMW_CACHE b_read=$got b_platter=$plat"
	echo "$got $plat" > $T/umltest/rmw.cache.$TAG
	dmsetup remove_all 2>/dev/null
	finish
	;;
rmw_write)
	# Sub-stripe writes into every full stripe the prep left recorded: a
	# 4K block of 'C' at each of RMW_OFFSETS blocks from every acknowledged
	# 'B' (default 2), never on a 'B' itself, so the blocks the probes check
	# are not rewritten.  Each is an
	# in-place read-modify-write of a stripe whose record names a stale
	# column -- the write that either repairs it, refuses it, or (before
	# either) folds the stale sector into the parity.
	do_mount $OPTS $MNTDEV
	allow_nodatacow
	stats "before writes"
	acked=0
	nr=0
	for i in $(seq 0 $((NOCOW_BLOCKS-1))); do
		for o in $(echo ${RMW_OFFSETS:-2} | tr , " "); do
			[ $((i * NOCOW_STRIDE + o)) -ge 0 ] || continue
			nr=$((nr+1))
			dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'C' |
			dd of=$MNT/nocow bs=4096 seek=$((i * NOCOW_STRIDE + o)) count=1 \
			   conv=notrunc,fsync status=none 2>/dev/null && acked=$((acked+1))
		done
	done
	sync
	log "RMW_WRITES acked=$acked of $nr"
	echo $acked > $T/umltest/rmw.acked.$TAG
	stats "after writes"
	kmsg "refusing a write|raid56:" 4
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
runscript)
	# Diagnostics: run $T/umltest/guest-script.$TAG inside the guest, with
	# the helpers above available, and log its output.
	btrfs device scan >/dev/null 2>&1
	[ -f $T/umltest/guest-script.$TAG ] && . $T/umltest/guest-script.$TAG 2>&1 |
		while read -r l; do log "script: $l"; done
	finish
	;;
nocow_platter)
	# Are the acknowledged blocks on the platters, as opposed to readable
	# through a filesystem that reconstructs whatever the record names?
	do_mount ro $MNTDEV
	python3 $T/umltest/nocow_platter.py $MNT/nocow $NOCOW_BLOCKS $NOCOW_STRIDE \
		$MNTDEV $T/umltest/nocow.acked.$TAG > /tmp/platter.out 2>&1
	umount $MNT || log "UMOUNT_FAIL"
	head -8 /tmp/platter.out | while read -r l; do log "platter: $l"; done
	n=$(sed -n 's/^PLATTER_MISSING \([0-9]*\) .*/\1/p' /tmp/platter.out)
	log "NOCOW_PLATTER missing=${n:-?}"
	echo "${n:-?}" > $T/umltest/nocow.bad.disk.$TAG
	finish
	;;
nocow_recover)
	# A normal read-write mount: this is what runs btrfs_wib_recover() and
	# scrubs every stripe the log recorded.
	[ "${RECOVER_LEGACY:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_recover_legacy \
			2>/dev/null || log "RECOVER_LEGACY_ARM_FAIL"
		log "error records will only be verified at mount (control)"
	}
	do_mount $OPTS $MNTDEV
	stats "after recovery"
	kmsg "write-intent" 8
	kmsg "scrub:" 6
	# How many records the recovery left.  One it retired is a stripe it
	# repaired; one it kept is a stripe whose redundancy it did not restore.
	sticky_rec=$(sed -n 's/.*sticky_blocks \([0-9]*\).*/\1/p' \
		/sys/fs/btrfs/*/raid56_write_intent 2>/dev/null | head -1)
	log "NOCOW_STICKY_AFTER_RECOVERY $sticky_rec"
	echo "${sticky_rec:-?}" > $T/umltest/nocow.sticky.recovery.$TAG
	# Read with every device present too, which is the ordinary nodatasum
	# exposure (the stale sector is returned as-is) rather than the
	# question this scenario asks.
	log "NOCOW_DIRECT bad=$(nocow_bad) of $NOCOW_BLOCKS"
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
flakey)
	# Like detach, but the drive only fails writes (reads still work), and
	# the filesystem is crashed while the drive is bad.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	log "fs devices: $(btrfs filesystem show $MNT 2>/dev/null | grep devid | tr -s ' ' | tr '\n' ';')"
	dd if=/dev/urandom of=$MNT/old bs=128K count=1 status=none; sync
	md5sum $MNT/old | awk '{print $1}' > $T/umltest/old.md5.$TAG
	writers_start
	sleep 2
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	sleep 3
	stats "with write errors"
	kmsg "write-intent|error" 4
	echo ${CRASH:-1} > /sys/module/btrfs/parameters/raid56_crash_point 2>/dev/null || log "CRASH_ARM_FAIL"
	dd if=/dev/urandom of=$MNT/new bs=4K count=1 conv=fsync status=none
	log "NO_CRASH"
	finish
	;;
degraded_log_full)
	# A degraded mount must stay writable however long it runs.
	#
	# RAID5 data AND metadata over four devices; one is removed and the
	# filesystem mounted degraded.  Every small in-place write into a full
	# stripe with a column on the missing device then leaves a record
	# naming that column, and none of them can retire while the device is
	# gone.  NREG regions of 4 MiB, each written in all three data columns,
	# is more than the log holds while anything is stale (82).  If the log
	# keeps every such record, it fills: small writes fail, and a metadata
	# write -- RAID5 metadata goes through the same log -- aborts the
	# transaction and leaves the filesystem read-only, with no way to run
	# 'btrfs replace'.  CONTROL=1 sets raid56_keep_naming_degraded=1.
	watchdog ${WATCH:-900}
	dm_setup
	mkfs.btrfs -K -q -f -d raid5 -m raid5 $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_keep_naming_degraded 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: records naming a missing device are kept"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	dd if=/dev/zero bs=1M count=$(( ${NREG:-120} * 4 + 16 )) status=none | tr '\000' 'A' > $MNT/nocow
	sync
	umount $MNT || { log "UMOUNT_FAIL"; finish; }
	# Pull the last device: remove its dm node and let btrfs forget it.
	last=$(( $(echo $DMDEVS | wc -w) - 1 ))
	dmsetup remove d$last || log "DM_REMOVE_FAIL"
	btrfs device scan --forget >/dev/null 2>&1
	btrfs device scan $(echo $DMDEVS | tr ' ' '\n' | grep -v "d$last\$") >/dev/null 2>&1
	do_mount $OPTS,degraded /dev/mapper/d0
	log "mounted degraded without d$last: $(grep " $MNT " /proc/mounts | awk '{print $4}')"
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > /tmp/bblock.dlf
	ok=0; eio=0
	for r in $(seq 0 $(( ${NREG:-120} - 1 ))); do
		for c in 0 1 2; do
			if dd if=/tmp/bblock.dlf of=$MNT/nocow bs=4096 oflag=direct conv=notrunc \
			      seek=$(( r * 1024 + c * 16 )) count=1 status=none 2>/dev/null; then
				ok=$((ok+1))
			else
				eio=$((eio+1))
			fi
		done
	done
	log "writes ok=$ok eio=$eio of $(( ${NREG:-120} * 3 ))"
	# Metadata: many small files, then a commit.
	meta=1
	mkdir -p $MNT/meta 2>/dev/null || meta=0
	for i in $(seq 1 200); do echo $i > $MNT/meta/f$i 2>/dev/null || meta=0; done
	sync
	btrfs filesystem sync $MNT 2>/dev/null || meta=0
	ro=0; grep " $MNT " /proc/mounts | awk '{print $4}' | grep -q "^ro" && ro=1
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	stats "end"
	log "DLF ok=$ok eio=$eio meta=$meta ro=$ro state=$(hv state) dropped=$(hv record_dropped) log_full=$(hv log_full) unack=$(hv unacknowledged)"
	echo "$ok $eio $meta $ro $(hv record_dropped) $(hv log_full)" > $T/umltest/dlf.$TAG
	kmsg "raid56|forced readonly|Transaction aborted" 8
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
replace_target_fail)
	# Does a device replace notice a target that does not take its writes?
	#
	# RAID5 over the first four devices, the fifth the replace target.  The
	# target fails every write past its first MiB (so the replace can start
	# and write its superblock), then 'btrfs replace start -B'.  The copy of
	# every sector goes to the target and is lost; a replace that finishes
	# anyway swaps in a device with holes where the source had data.
	# CONTROL=1 sets scrub_replace_ignores_write_errors=1, the old
	# behaviour: then a read-only scrub afterwards finds them.
	watchdog ${WATCH:-900}
	dm_setup
	set -- $DMDEVS
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $1 $2 $3 $4 || { log "MKFS_FAIL"; finish; }
	TGT=$5
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 $4 >/dev/null 2>&1
	do_mount $OPTS $1
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/scrub_replace_ignores_write_errors 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: target write errors ignored"
	}
	head -c 96M /dev/urandom > $MNT/data
	sync
	want=$(md5sum < $MNT/data | awk '{print $1}')
	d=$(echo $DEVS | awk '{print $5}')
	sz=$(blockdev --getsz $d)
	printf '0 2048 linear %s 0\n2048 %d flakey %s 2048 0 1000 1 error_writes\n' $d $((sz - 2048)) $d |
		{ dmsetup suspend --nolockfs --noflush d4 && dmsetup reload d4 && dmsetup resume d4; } ||
		log "DM_RELOAD_FAIL d4"
	log "target $TGT fails writes past 1 MiB"
	btrfs replace start -B -f 1 $TGT $MNT > /tmp/rep.out 2>&1
	rrc=$?
	log "replace rc=$rrc: $(tr '\n' ' ' < /tmp/rep.out | cut -c1-200)"
	log "replace status: $(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ' | cut -c1-160)"
	dm_heal 4
	inuse=0; btrfs filesystem show $MNT 2>/dev/null | grep -q "$TGT" && inuse=1
	log "target in use after replace: $inuse"
	# Scrub read-only BEFORE reading the file: a read repairs what it
	# finds wrong (from the parity, written back to the new device) and
	# would leave the scrub nothing to count.
	btrfs scrub start -Br $MNT > /tmp/scrub.out 2>&1
	csum=$(grep -o "csum=[0-9]*" /tmp/scrub.out | head -1 | cut -d= -f2)
	echo 3 > /proc/sys/vm/drop_caches
	got=$(md5sum < $MNT/data 2>/dev/null | awk '{print $1}')
	log "scrub: $(tr '\n' ' ' < /tmp/scrub.out | grep -o 'Error summary:.*' | cut -c1-120)"
	kmsg "replace|scrub: device replace could not write" 6
	log "RTF rrc=$rrc inuse=$inuse md5ok=$([ "$got" = "$want" ] && echo 1 || echo 0) csum=${csum:-0}"
	echo "$rrc $inuse $([ "$got" = "$want" ] && echo 1 || echo 0) ${csum:-0}" > $T/umltest/rtf.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
degraded_fresh)
	# A RAID6 array that is degraded from the start, writing into space
	# that has never been written.  The parity of a never-written vertical
	# stripe is whatever was on the disk, so P and Q disagree; a missing
	# device makes every such stripe need reconstruction, and free space
	# has no checksum to vouch for the result.  If the Q cross-check in
	# recover_vertical() rejects that, a degraded array cannot write into
	# fresh space at all.
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS,degraded $MNTDEV
	watchdog 120
	fails=0
	for i in $(seq 1 20); do
		dd if=/dev/urandom of=$MNT/f$i bs=4K count=1 conv=fsync status=none \
			2>/dev/null || fails=$((fails+1))
	done
	sync 2>/dev/null || fails=$((fails+1))
	log "degraded fresh sub-stripe writes: $fails of 21 failed"
	btrfs filesystem df $MNT 2>&1 | while read -r l; do log "df: $l"; done
	# Force a genuine read-modify-write: overwrite part of an existing
	# extent in place, which cannot be redirected to fresh space.
	dd if=/dev/urandom of=$MNT/big bs=1M count=4 conv=fsync status=none 2>/dev/null
	sync
	allow_nodatacow
	chattr +C $MNT/nocow 2>/dev/null
	dd if=/dev/zero of=$MNT/nocow bs=1M count=2 conv=fsync status=none 2>/dev/null
	sync
	dd if=/dev/urandom of=$MNT/nocow bs=4K count=4 seek=7 conv=notrunc,fsync \
		status=none 2>/dev/null || log "INPLACE_WRITE_FAIL"
	sync
	[ "$fails" = 0 ] && log "DEGRADED_FRESH_OK" || log "DEGRADED_FRESH_FAIL"
	kmsg "does not match the Q syndrome" 3
	stats
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
degraded_write)
	# The array is already degraded (device omitted by the host) while
	# writes happen and the crash hits.  Recovery cannot regenerate the
	# missing device's sectors; committed data on the present devices
	# must still be intact and the loss must be detected, not silent.
	do_mount $OPTS,degraded $MNTDEV
	stats
	dd if=/dev/urandom of=$MNT/old2 bs=128K count=1 status=none; sync
	md5sum $MNT/old2 | awk '{print $1}' > $T/umltest/old2.md5.$TAG
	echo ${CRASH:-1} > /sys/module/btrfs/parameters/raid56_crash_point 2>/dev/null || log "CRASH_ARM_FAIL"
	dd if=/dev/urandom of=$MNT/new bs=4K count=1 conv=fsync status=none
	log "NO_CRASH"
	finish
	;;
degraded_verify)
	do_mount "$OPTS${OMITTED:+,degraded}" $MNTDEV
	kmsg "write-intent|regenerat" 4
	stats
	verify_old FULL
	echo 3 > /proc/sys/vm/drop_caches
	M=$(md5sum $MNT/old2 2>&1 | awk '{print $1}')
	[ "$M" = "$(cat $T/umltest/old2.md5.$TAG)" ] && log "OLD2_READ_OK" || { log "OLD2_READ_FAIL md5=$M"; kmsg "csum|error" 3; }
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
locate)
	# Print the physical location of the first sector of 'old' on every
	# device, for host-side corruption tests.
	do_mount $OPTS,ro $MNTDEV
	L=$(filefrag -v $MNT/old | sed -n 4p | awk -F: '{print $3}' | awk '{print $1}' | tr -d '.')
	umount $MNT
	log "old logical block $L"
	btrfs-map-logical -l $((L * 4096)) -b 4096 $MNTDEV 2>&1 | while read -r l; do log "map: $l"; done
	finish
	;;
writers)
	# Plain concurrent writer workload for a fixed time, no failure.
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DEVS || { log "MKFS_FAIL"; finish; }
	do_mount $OPTS $MNTDEV
	watchdog ${WATCH:-100}
	writers_start
	sleep ${DURATION:-30}
	writers_stop
	log "writers finished: $(ls $MNT | wc -l) files"
	sync
	stats
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
stale_parity)
	# A device rejects writes for a while.  Every sector it does not take
	# stays behind while the rest of its vertical stripe moves on, and
	# because a single fault is within what RAID5/6 tolerates the writes
	# are accepted: the writer is told they succeeded.  Nothing tracks
	# those sectors afterwards.  For a file with no checksums a later read
	# returns the stale bytes with no error at all, and the retry added to
	# the write path only removes the case where the device recovers in
	# time.  This reproduces that exposure; see the "remaining exposures"
	# section of tools/testing/btrfs/raid56_redundancy_model.py.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	mkdir -p $MNT/nc; chattr +C $MNT/nc 2>/dev/null
	dd if=/dev/urandom of=$MNT/nc/f bs=64K count=24 conv=fsync status=none
	sync
	md5sum $MNT/nc/f | awk '{print $1}' > $T/umltest/old.md5.$TAG
	log "nocow file md5 $(cat $T/umltest/old.md5.$TAG)"
	# What each 4K block may read back as afterwards: the original, and for
	# a rewritten block the rewrite -- required if it was acknowledged,
	# allowed if it was refused (the kernel refuses a write it cannot make
	# safe, and a refused write promised nothing).
	cp $MNT/nc/f $T/umltest/sp.orig.$TAG
	rm -f $T/umltest/sp.new.*.$TAG $T/umltest/sp.acked.$TAG
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	# Sub-stripe rewrites: data and Q land, P on the bad device does not.
	for off in 0 2 4 6 8 10 12 14 16 18 20 22; do
		dd if=/dev/urandom of=$T/umltest/sp.new.$((off * 16)).$TAG bs=4K count=1 status=none
		dd if=$T/umltest/sp.new.$((off * 16)).$TAG of=$MNT/nc/f bs=4K count=1 seek=$((off * 16)) \
			conv=notrunc,fsync status=none 2>/dev/null &&
			echo $((off * 16)) >> $T/umltest/sp.acked.$TAG
	done
	sync
	log "rewrites acknowledged: $(wc -l < $T/umltest/sp.acked.$TAG 2>/dev/null) of 12"
	md5sum $MNT/nc/f | awk '{print $1}' > $T/umltest/new.md5.$TAG
	log "after rewrites md5 $(cat $T/umltest/new.md5.$TAG)"
	dm_heal $FAIL; log "healed device $FAIL"
	kmsg "raid56:|does not match the Q syndrome" 6
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all
	finish
	;;
stale_parity_verify)
	# The array comes up with one data device missing: every read of the
	# file needs a single-erasure rebuild.
	dm_setup
	dm_scan
	do_mount "$OPTS${OMITTED:+,degraded}" /dev/mapper/d0
	echo 3 > /proc/sys/vm/drop_caches
	V=$(python3 - "$MNT/nc/f" "$T/umltest" "$TAG" <<'PY'
import os, sys
path, d, tag = sys.argv[1:4]
BS = 4096
orig = open(f'{d}/sp.orig.{tag}', 'rb').read()
try:
    acked = {int(x) for x in open(f'{d}/sp.acked.{tag}').read().split()}
except OSError:
    acked = set()
try:
    got = open(path, 'rb').read()
except OSError:
    print('REFUSED'); sys.exit()
if len(got) != len(orig):
    print('SILENT short'); sys.exit()
bad = []
for b in range(len(orig) // BS):
    blk, old = got[b*BS:(b+1)*BS], orig[b*BS:(b+1)*BS]
    f = f'{d}/sp.new.{b}.{tag}'
    new = open(f, 'rb').read() if os.path.exists(f) else None
    if new is None:
        ok = blk == old
    elif b in acked:
        ok = blk == new
    else:
        ok = blk in (old, new)
    if not ok:
        bad.append(b)
print('CORRECT' if not bad else 'SILENT blocks ' + ' '.join(map(str, bad[:8])))
PY
)
	case "$V" in
	REFUSED*) log "STALE_SECTOR_READ_REFUSED (detected)";;
	CORRECT) log "STALE_SECTOR_READ_CORRECT";;
	*) log "STALE_SECTOR_SILENT_CORRUPTION $V (known exposure)";;
	esac
	kmsg "does not match the Q syndrome|csum|error" 6
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all
	finish
	;;
devstats)
	# Directly exercise the new accounting: one device fails writes for a
	# while (transient), so the RMW retry should fire and the device error
	# counters should move.  Previously raid56.c touched neither.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	dd if=/dev/urandom of=$MNT/old bs=128K count=1 status=none; sync
	md5sum $MNT/old | awk '{print $1}' > $T/umltest/old.md5.$TAG
	log "stats before: $(btrfs device stats $MNT | tr '\n' ' ')"
	dm_error_writes $FAIL; log "write errors on device $FAIL"
	writers_start
	sleep 4
	dm_heal $FAIL; log "healed device $FAIL"
	sleep 2
	writers_stop
	sync
	log "stats after: $(btrfs device stats $MNT | grep -v ' 0$' | tr '\n' ' ')"
	kmsg "raid56: (write|read) error|raid56: retrying" 6
	if dmesg | grep -q "raid56: retrying"; then log "RETRY_OBSERVED"; else log "RETRY_NOT_OBSERVED"; fi
	if btrfs device stats $MNT | grep -q "write_io_errs *[1-9]"; then log "DEVSTATS_OBSERVED"; else log "DEVSTATS_NOT_OBSERVED"; fi
	stats
	verify_old FULL
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all
	finish
	;;
scrub)
	do_mount $OPTS $MNTDEV
	btrfs scrub start -B -d $MNT 2>&1 | grep -E "Error summary|corrected|uncorrectable|unverified" | while read -r l; do log "scrub: $l"; done
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
*)
	log "UNKNOWN MODE"
	finish
	;;
esac
