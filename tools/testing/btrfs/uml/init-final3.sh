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
# The root is the host's (hostfs), and so is its /tmp: guests running at once
# would share the scratch files the scenarios keep there under fixed names,
# and overwrite each other's.  A /tmp of this guest's own, unless the test
# directory is under it.
case $T/ in
/tmp/*) ;;
*) mount -t tmpfs tmpfs /tmp || log "TMPFS_FAIL: /tmp is the host's";;
esac
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
dm_drop_writes() {	# index: every write, flushes too, "succeeds" and is thrown away
	local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
	dm_reload d$1 "0 $(blockdev --getsz $d) flakey $d 0 0 1000 1 drop_writes"
}
dm_heal() {
	local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
	dm_reload d$1 "0 $(blockdev --getsz $d) linear $d 0"
}
dm_bad_sectors() {	# index physical-offset...: those 4 KiB fail every IO, the rest maps through
	local i=$1 d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}') at=0 t="" s o
	local sz=$(blockdev --getsz $d)
	shift
	for o in $(printf '%s\n' "$@" | sort -n); do
		s=$(( o / 512 ))
		[ $s -gt $at ] && t="$t$at $((s - at)) linear $d $at\n"
		t="$t$s 8 error\n"
		at=$((s + 8))
	done
	[ $sz -gt $at ] && t="$t$at $((sz - at)) linear $d $at\n"
	printf "$t" | { dmsetup suspend --nolockfs --noflush d$i && dmsetup reload d$i &&
			 dmsetup resume d$i; } || log "DM_RELOAD_FAIL d$i"
}
dm_error_writes_range() {	# index byte-offset byte-length: writes there fail, the rest maps through
	local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
	local sz=$(blockdev --getsz $d) s=$(( $2 / 512 )) n=$(( $3 / 512 ))
	printf '0 %d linear %s 0\n%d %d flakey %s %d 0 1000 1 error_writes\n%d %d linear %s %d\n' \
		$s $d $s $n $d $s $((s + n)) $((sz - s - n)) $d $((s + n)) |
		{ dmsetup suspend --nolockfs --noflush d$1 && dmsetup reload d$1 &&
		  dmsetup resume d$1; } || log "DM_RELOAD_FAIL d$1"
}
dm_error_reads_range() {	# index byte-offset byte-length: reads there fail, the rest not
	local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
	local sz=$(blockdev --getsz $d) s=$(( $2 / 512 )) n=$(( $3 / 512 ))
	printf '0 %d linear %s 0\n%d %d flakey %s %d 0 1000 1 error_reads\n%d %d linear %s %d\n' \
		$s $d $s $n $d $s $((s + n)) $((sz - s - n)) $d $((s + n)) |
		{ dmsetup suspend --nolockfs --noflush d$1 && dmsetup reload d$1 &&
		  dmsetup resume d$1; } || log "DM_RELOAD_FAIL d$1"
}
# One 4 KiB block of $MNT/nocow, read cold: echoes "<bytes read> <bytes of 'A'>".
nocow_blk() {	# file-offset
	echo 3 > /proc/sys/vm/drop_caches
	local n a
	n=$(dd if=$MNT/nocow bs=4096 skip=$(( $1 / 4096 )) count=1 status=none 2>/dev/null | wc -c)
	a=$(dd if=$MNT/nocow bs=4096 skip=$(( $1 / 4096 )) count=1 status=none 2>/dev/null |
	    tr -cd 'A' | wc -c)
	echo "$n $a"
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
# readd_flush_*: blocks overwritten, one per full stripe (NOCOW_FS_BLOCKS
# apart), so that no overwrite reads a column another one left on the device
# that is dropping writes.
RF_BLOCKS=${RF_BLOCKS:-24}
# torn_readd_*: blocks overwritten, each in a region (4 MiB of address space,
# one log entry) of its own -- more regions than a block that names the
# failed device can describe (82), fewer than one that names nothing can
# (165) -- TR_STRIDE full stripes apart: 23, one more than 4 MiB holds on four
# devices, and odd, so that the column a block lands in rotates.
TR_REGIONS=${TR_REGIONS:-96}
TR_STRIDE=$(( 23 * NOCOW_FS_BLOCKS ))
# Read back every row torn_readd_prep's overwrites rewrote the parity of,
# cold, and score the blocks: those on OMITTED (rebuilt from that parity) as
# ok, eio or wrong (TR_WRONG), those on FAIL as old ('A' where its write was
# dropped: fail_old, not scored), and every other one, read directly, as bad
# unless it is what was acknowledged (TR_DIRECT_BAD).  Sets checked, ok, eio,
# wrong, torn_rows, fail_old and direct_bad.
tr_score() {
	echo 3 > /proc/sys/vm/drop_caches
	acked=" $(tr '\n' ' ' < $T/umltest/tr.acked.$TAG 2>/dev/null) "
	checked=0; ok=0; eio=0; wrong=0; torn_rows=0; fail_old=0; direct_bad=0
	blk() {	# file-block want -> ok | eio | wrong
		if dd if=$MNT/nocow of=$T/umltest/blk.$TAG bs=4096 count=1 skip=$1 \
		      status=none 2>/dev/null; then
			[ "$(tr -cd "$2" < $T/umltest/blk.$TAG | wc -c)" = 4096 ] && echo ok ||
				echo wrong
		else
			echo eio
		fi
	}
	while read -r i wfb wcol pdev mates; do
		[ -n "$mates" ] && [ "$wcol" -ge 0 ] 2>/dev/null || continue
		cols=($mates)
		wdev=${cols[$wcol]##*:}
		torn=0
		{ [ "$pdev" = "$FAIL" ] || [ "$wdev" = "$FAIL" ]; } && torn=1
		torn_rows=$((torn_rows + torn))
		for m in $mates; do
			b=${m%%:*}; d=${m##*:}
			[ "$b" -ge 0 ] || continue
			want=A
			[ "$b" = "$wfb" ] && case "$acked" in *" $i "*) want=B;; esac
			v=$(blk $b $want)
			if [ "$d" = "$FAIL" ]; then
				[ $v = wrong ] && fail_old=$((fail_old + 1))
			elif [ "$d" = "$OMITTED" ]; then
				checked=$((checked + 1))
				case $v in
				ok) ok=$((ok + 1));;
				eio) eio=$((eio + 1));;
				*) wrong=$((wrong + 1)); log "TR_WRONG row $i block $b torn=$torn";;
				esac
			elif [ $v != ok ]; then
				direct_bad=$((direct_bad + 1))
				log "TR_DIRECT_BAD row $i block $b $v"
			fi
		done
	done < $T/umltest/tr.rows.$TAG
	rm -f $T/umltest/blk.$TAG
}
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
	# RETRY_GIVE_UP=1 (with a short raid56_repair_delay_ms): the device
	# goes on failing until every repair queued on it has given up.  Each
	# retry is a write recorded after the last transaction commit -- a
	# repair takes none -- into a stripe whose record names the column,
	# and it finishes, failed: the log goes on listing it in flight until
	# something flushes every device and writes the log again.
	if [ "${RETRY_GIVE_UP:-0}" = 1 ]; then
		W=$(ls /sys/fs/btrfs/*-*-*/raid56_write_intent 2>/dev/null | head -1)
		H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
		wv() { awk -v k=$1 '$1 == k {print $2}' $W 2>/dev/null; }
		hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
		t=0
		while [ $t -lt 120 ]; do
			sleep 1; t=$((t + 1))
			gu=$(hv repair_gave_up)
			[ "${gu:-0}" -gt 0 ] 2>/dev/null && [ "$(wv repair_queued)" = \
			  "$(( $(wv repair_ok) + $(wv repair_failed) + $(wv repair_skipped) ))" ] && break
		done
		log "repairs after ${t}s: queued=$(wv repair_queued) ok=$(wv repair_ok)" \
		    "failed=$(wv repair_failed) skipped=$(wv repair_skipped) gave_up=$(hv repair_gave_up)"
	fi
	dm_heal $FAIL; log "healed device $FAIL"
	# Give a repair the kernel queued on the failed writes time to find the
	# device healthy again (rmw_repair.sh, the trigger arms).
	[ "${SETTLE:-0}" -gt 0 ] && sleep $SETTLE
	stats "after write errors"
	# REMOUNT_RO=1: read-only before the unmount, which then writes
	# nothing -- what the log lists now is what the next mount reads,
	# as after a crash of the read-only filesystem.
	if [ "${REMOUNT_RO:-0}" = 1 ]; then
		mount -o remount,ro $MNT && log "remounted read-only" || log "REMOUNT_FAIL"
		stats "after the remount"
	fi
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
forced_stale)
	# Does scrub keep a block the write-intent log names stale in error when
	# the rebuild that should replace it cannot run, or does its last-resort
	# re-read of the stale device take the old bytes back?
	#
	# RAID5 data over four devices -- three data columns and P per full
	# stripe, the columns rotating by one device per full stripe -- and
	# raid1 metadata.  Two devices, chosen by chunk stripe so the layout is
	# known:
	#
	#   W  chunk stripe 0, failing writes while one 4K block of its data
	#      column is overwritten with 'B' in each full stripe whose P is on
	#      V.  One fault, within RAID5's tolerance: acknowledged, P updated,
	#      W's column recorded stale.  Healed afterwards, so W READS FINE --
	#      it just still holds the old 'A'.
	#   V  chunk stripe 1, healthy while that happens; then failing READS
	#      (not writes) for the first scrub.  In exactly those full stripes
	#      V holds the only copy of what was acknowledged: the parity.
	#
	# The file is nodatacow, so nothing but the record says those blocks
	# are stale, and the automatic repair is off (raid56_no_repair_on_fault)
	# so that the scrub is what acts on it.  The scrub marks W's column in
	# error from the record (scrub_mark_wib_stale_sectors()) and asks for a
	# rebuild, which needs P and fails.  Its last-resort pass then re-reads
	# every mirror, W's own first: that brings the stale 'A' back, and
	# verification, with no checksum to go on, clears the error.  Believed,
	# the stripe counts as repaired, so its parity is regenerated from the
	# 'A' (V takes writes) and the record retired: 'B' is gone from
	# everywhere and a read returns 'A' without an error.
	#
	# Kept in error, the full stripe is reported unrepaired, P keeps 'B',
	# the record stays, a read rebuilds 'B', and a second scrub with V
	# healthy puts 'B' on W's platter.
	#
	# The measurement is the parity itself.  Vertical 0 of each full stripe
	# holds W's overwritten block and two untouched 'A' blocks, so P there
	# reads 'B' (A^A^B) while it still describes the acknowledged write and
	# 'A' (A^A^A) once it has been regenerated from the stale bytes.
	#
	# CONTROL=1 sets raid56_scrub_reread_trusts_stale=1.
	#
	# FST_REPLACE=1 goes on from there, with a fifth device as the replace
	# target: V, still failing reads, is replaced after the first scrub, as
	# the alerts recommend for a failing device.  The replace scrubs V's
	# parity of those full stripes, cannot rebuild W's column for the same
	# reason, keeps it in error and declines to regenerate the parity -- so
	# nothing would write the target's parity of them, and the record, which
	# names W's column and no bad parity, would from then on describe the
	# target as good parity.  scrub_replace_copy_parity() copies V's parity
	# instead, and where V does not return it, as here, writes zeros and
	# records that parity stale too.  The stripe is then undecidable, which
	# it is -- 'B' was only ever in V's parity -- so a read of W's block, and
	# of the untouched 'A' next to it in the same column, fails with EIO, and
	# a second scrub leaves W's column alone.  CONTROL=1 there sets
	# raid56_wf_replace_skips_refused_parity=1 instead: the target keeps its
	# zeros as good parity, both reads return zeros (0^A^A) without an error,
	# and the second scrub, finding the stripe proven, writes zeros over
	# W's whole column -- over 'A's that were right -- and retires the
	# record.
	watchdog ${WATCH:-900}
	dm_setup
	set -- $DMDEVS
	[ $# = $(( 4 + ${FST_REPLACE:-0} )) ] ||
		{ log "NEED_$(( 4 + ${FST_REPLACE:-0} ))_DEVICES"; finish; }
	FST_TGT=${5:-}
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $1 $2 $3 $4 || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	echo 1 > /sys/module/btrfs/parameters/raid56_no_repair_on_fault 2>/dev/null ||
		log "NOREPAIR_KNOB_FAIL"
	[ "${CONTROL:-0}" = 1 ] && [ "${FST_REPLACE:-0}" != 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_scrub_reread_trusts_stale 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: a re-read of the stale device clears the errors the record forced"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	lsattr $MNT/nocow 2>/dev/null | grep -q C || log "NOT_NODATACOW"
	# Whole full stripes (3 x 64 KiB): a full stripe only partly this file
	# can end the device's scrub early (scrub_raid56_parity_stripe()
	# returns the "no extent" 1 of its last data column), and then says
	# nothing about this.
	dd if=/dev/zero bs=64K count=$(( ${FST_ROWS:-20} * 3 )) status=none | tr '\000' 'A' > $MNT/nocow
	sync
	LAY=$T/umltest/fst.layout.$TAG
	# First line: W, V (dm indices), ND, NUM, ALIGNED.  Then one line per
	# full stripe where W holds a data column and V the parity:
	# "ROW n file_offset physical_on_W physical_on_V".  Column c of full
	# stripe n is on chunk stripe (c + n) % NUM, as btrfs_map_block() and
	# scrub_raid56_parity_stripe() lay it out, and its physical address on
	# that device is the dev extent's offset plus n x 64 KiB.
	python3 - $MNT/nocow /dev/mapper/d0 > $LAY 2>&1 <<'PY'
import re, subprocess, sys
path, dev = sys.argv[1:3]
SL = 65536
ext = []
for line in subprocess.run(['filefrag', '-v', '-b4096', path],
                           capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*\d+:\s+(\d+)\.\.\s*(\d+):\s+(\d+)\.\.\s*(\d+):', line)
    if m:
        f0, f1, p0, _ = (int(x) * 4096 for x in m.groups())
        if ext and ext[-1][1] == f0 and ext[-1][2] + (ext[-1][1] - ext[-1][0]) == p0:
            ext[-1] = (ext[-1][0], f1 + 4096, ext[-1][2])
        else:
            ext.append((f0, f1 + 4096, p0))
if not ext:
    sys.exit('no extents')
out = subprocess.run(['btrfs', 'inspect-internal', 'dump-tree', '-t', 'chunk', dev],
                     capture_output=True, text=True).stdout
chunks, cur = [], None
for line in out.splitlines():
    m = re.search(r'CHUNK_ITEM (\d+)\)', line)
    if m:
        cur = {'start': int(m.group(1)), 'stripes': []}
        chunks.append(cur)
        continue
    if cur is None:
        continue
    m = re.search(r'length (\d+) owner \d+ stripe_len (\d+) type (\S+)', line)
    if m:
        cur['len'], cur['type'] = int(m.group(1)), m.group(3)
    m = re.search(r'stripe (\d+) devid (\d+) offset (\d+)', line)
    if m:
        cur['stripes'].append((int(m.group(2)), int(m.group(3))))
devpath = {}
show = subprocess.run(['btrfs', 'filesystem', 'show', dev], capture_output=True,
                      text=True).stdout
for m in re.finditer(r'devid\s+(\d+)\s+size.*?path\s+(\S+)', show):
    devpath[int(m.group(1))] = m.group(2)
c = next((c for c in chunks if 'len' in c and
          c['start'] <= ext[0][2] < c['start'] + c['len']), None)
if c is None or 'RAID5' not in c['type'] or len(c['stripes']) != 4:
    sys.exit('file not in a four-device RAID5 chunk')
num = len(c['stripes'])
nd = num - 1
fsl = nd * SL
iW, iV = 0, 1
def dmidx(k):
    return int(re.search(r'(\d+)$', devpath[c['stripes'][k][0]]).group(1))
aligned, rows = 1, []
for f0, f1, p0 in ext:
    if not (c['start'] <= p0 and p0 + (f1 - f0) <= c['start'] + c['len']):
        aligned = 0
        continue
    first = (p0 - c['start']) // fsl
    last = (p0 + (f1 - f0) - 1 - c['start']) // fsl
    for n in range(first, last + 1):
        fss = c['start'] + n * fsl
        if fss < p0 or fss + fsl > p0 + (f1 - f0):
            aligned = 0
            continue
        colW, colV = (iW - n) % num, (iV - n) % num
        if colW >= nd or colV != nd:
            continue
        rows.append((n, f0 + fss - p0 + colW * SL,
                     c['stripes'][iW][1] + n * SL,
                     c['stripes'][iV][1] + n * SL))
print(f'W={dmidx(iW)} V={dmidx(iV)} ND={nd} NUM={num} ALIGNED={aligned}')
for r in rows:
    print('ROW %d %d %d %d' % r)
PY
	head -1 $LAY | grep -q '^W=' || { log "LAYOUT_FAIL $(head -3 $LAY | tr '\n' ' ')"; finish; }
	eval "$(head -1 $LAY)"
	log "layout: $(head -1 $LAY), $(grep -c '^ROW ' $LAY) rows with W holding data and V the parity"
	[ "$ALIGNED" = 1 ] || { log "LAYOUT_UNALIGNED"; finish; }
	# Past the first MiB only, for both faults: the superblock and the
	# write-intent log (at 512 KiB) keep working, so what is under test is
	# the scrub and not whether the log survives.
	flakey_past_1m() {	# dm-index feature
		local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
		local sz=$(blockdev --getsz $d)
		printf '0 2048 linear %s 0\n2048 %d flakey %s 2048 0 1000 1 %s\n' \
			$d $((sz - 2048)) $d $2 |
			{ dmsetup suspend --nolockfs --noflush d$1 && dmsetup reload d$1 &&
			  dmsetup resume d$1; } || log "DM_RELOAD_FAIL d$1"
	}
	flakey_past_1m $W error_writes
	log "d$W fails writes past 1 MiB"
	# O_DIRECT, one block at a time: each write is its own read-modify-
	# write and is done by the time dd returns, so nothing is left for a
	# transaction commit to push at W before it is healed.
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > /tmp/bblock.fst
	grep '^ROW ' $LAY > /tmp/fst.rows
	rm -f $T/umltest/fst.acked.$TAG; touch $T/umltest/fst.acked.$TAG
	while read -r _ n fo pw pv; do
		dd if=/tmp/bblock.fst of=$MNT/nocow bs=4096 seek=$((fo / 4096)) count=1 \
		   conv=notrunc oflag=direct status=none 2>/dev/null &&
			echo $n >> $T/umltest/fst.acked.$TAG
	done < /tmp/fst.rows
	dm_heal $W; log "healed d$W"
	sync
	log "overwrites acknowledged: $(wc -l < $T/umltest/fst.acked.$TAG) of $(wc -l < /tmp/fst.rows)"
	H=$(ls -d /sys/fs/btrfs/*-*-* 2>/dev/null | head -1)
	hv() { awk -v k=$1 '$1 == k {print $2}' $H/raid56_health 2>/dev/null; }
	rec0=$(hv recorded_blocks)
	log "before scrub: stale_marks=$(hv stale_marks) recorded_blocks=$rec0"
	blk() {	# dd input args -> B, A, Z (zeros), E (read failed) or X (anything else)
		rm -f /tmp/fst.blk
		dd "$@" of=/tmp/fst.blk bs=4096 count=1 status=none 2>/dev/null &&
			[ "$(stat -c %s /tmp/fst.blk)" = 4096 ] || { echo E; return; }
		if [ "$(tr -cd 'B' < /tmp/fst.blk | wc -c)" = 4096 ]; then echo B
		elif [ "$(tr -cd 'A' < /tmp/fst.blk | wc -c)" = 4096 ]; then echo A
		elif [ "$(tr -d '\000' < /tmp/fst.blk | wc -c)" = 0 ]; then echo Z
		else echo X; fi
	}
	# The first scrub, with the parity of those full stripes unreadable.
	flakey_past_1m $V error_reads
	log "d$V fails reads past 1 MiB"
	btrfs scrub start -B $MNT > /tmp/fst.scrub1 2>&1
	log "scrub 1 rc=$?: $(grep -E 'Error summary|ERROR' /tmp/fst.scrub1 | tr '\n' ' ' | cut -c1-240)"
	dm_heal $V; log "healed d$V"
	rec1=$(hv recorded_blocks)
	# Rate limited, so a count of messages, not of sectors: it only has to
	# be there.  Nothing before this scrub prints it.
	nkept=$(dmesg | grep -c 're-read from mirror [0-9]* still hold')
	log "after scrub 1: stale_marks=$(hv stale_marks) recorded_blocks=$rec1 refusals=$nkept"
	kmsg "re-read from mirror|rebuilding [0-9]+ sector|unrepaired sectors" 8
	# What scrub 1 left on the platters: W's block (still 'A' in both arms,
	# or the rebuild ran after all and the scenario proves nothing) and the
	# parity on V ('B' kept, 'A' regenerated from the stale block).
	while read -r _ n fo pw pv; do
		grep -qx $n $T/umltest/fst.acked.$TAG || continue
		echo "$n $(blk if=/dev/mapper/d$W iflag=direct skip=$((pw / 4096))) $(blk if=/dev/mapper/d$V iflag=direct skip=$((pv / 4096)))"
	done < /tmp/fst.rows > /tmp/fst.plat1
	if [ "${FST_REPLACE:-0}" = 1 ]; then
		# The replace arms.  V fails reads again, as it did for the first
		# scrub: it was healthy only while the platters above were read,
		# with nothing reading through the filesystem.
		[ "${CONTROL:-0}" = 1 ] && {
			echo 1 > /sys/module/btrfs/parameters/raid56_wf_replace_skips_refused_parity 2>/dev/null ||
				log "CONTROL_KNOB_FAIL"
			log "control: the replace leaves the target's parity of a full stripe it declines to regenerate as it was"
		}
		flakey_past_1m $V error_reads
		log "d$V fails reads past 1 MiB again; replacing it with $FST_TGT"
		btrfs replace start -B -K -f /dev/mapper/d$V $FST_TGT $MNT > /tmp/fsr.rep 2>&1
		rrc=$?
		log "replace rc=$rrc: $(tr '\n' ' ' < /tmp/fsr.rep | cut -c1-200)"
		st=$(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ')
		log "replace status: $(echo "$st" | cut -c1-160)"
		unc=$(echo "$st" | grep -o '[0-9]* uncorr' | awk '{print $1}')
		# Out of the filesystem now, if the replace finished.
		dm_heal $V; log "healed d$V"
		nlost=$(dmesg | grep -c 'could neither regenerate nor copy [0-9]* sector(s) of parity')
		unco=$(hv replace_uncopyable)
		log "after the replace: stale_marks=$(hv stale_marks) recorded_blocks=$(hv recorded_blocks) replace_uncopyable=$unco parity_lost_msgs=$nlost"
		kmsg "neither regenerate nor copy|unrepaired sectors|replace" 8
		# What a reader gets now: the acknowledged block, and the 'A' next
		# to it in W's column, which no write ever touched.
		echo 3 > /proc/sys/vm/drop_caches
		while read -r _ n fo pw pv; do
			grep -qx $n $T/umltest/fst.acked.$TAG || continue
			echo "$n $(blk if=$MNT/nocow skip=$((fo / 4096))) $(blk if=$MNT/nocow skip=$((fo / 4096 + 1)))"
		done < /tmp/fst.rows > /tmp/fsr.read
		# A second scrub, with every device of the filesystem reading.
		btrfs scrub start -B $MNT > /tmp/fsr.scrub2 2>&1
		log "scrub 2 rc=$?: $(grep -E 'Error summary|ERROR' /tmp/fsr.scrub2 | tr '\n' ' ' | cut -c1-240)"
		rec2=$(hv recorded_blocks)
		log "after scrub 2: stale_marks=$(hv stale_marks) recorded_blocks=$rec2"
		kmsg "left untouched|unrepaired sectors" 4
		umount $MNT || log "UMOUNT_FAIL"
		# Per acknowledged row: what scrub 1 left, what the reads returned,
		# and what scrub 2 left on W's platter (the block, then its
		# neighbour) -- 'B' or the stale 'A' in the block, 'A' in the
		# neighbour, or something nothing ever wrote there.  The target's
		# parity is logged for the record: zeros in both arms.
		rows=0; acked=0; stale1=0; kept=0; ok=0; eio=0; wrong=0; changed=0
		while read -r _ n fo pw pv; do
			rows=$((rows+1))
			grep -qx $n $T/umltest/fst.acked.$TAG || { log "FSR row $n refused"; continue; }
			acked=$((acked+1))
			w1=$(awk -v n=$n '$1 == n {print $2}' /tmp/fst.plat1)
			p1=$(awk -v n=$n '$1 == n {print $3}' /tmp/fst.plat1)
			rb=$(awk -v n=$n '$1 == n {print $2}' /tmp/fsr.read)
			rn=$(awk -v n=$n '$1 == n {print $3}' /tmp/fsr.read)
			wb=$(blk if=/dev/mapper/d$W iflag=direct skip=$((pw / 4096)))
			wn=$(blk if=/dev/mapper/d$W iflag=direct skip=$((pw / 4096 + 1)))
			tp=$(blk if=$FST_TGT iflag=direct skip=$((pv / 4096)))
			[ "$w1" = A ] && stale1=$((stale1+1))
			[ "$p1" = B ] && kept=$((kept+1))
			case $rb in B) ok=$((ok+1));; E) eio=$((eio+1));; *) wrong=$((wrong+1));; esac
			case $rn in A) ok=$((ok+1));; E) eio=$((eio+1));; *) wrong=$((wrong+1));; esac
			case $wb in A|B) ;; *) changed=$((changed+1));; esac
			[ "$wn" = A ] || changed=$((changed+1))
			log "FSR row $n scrub1: W=$w1 parity=$p1 replaced: read=$rb/$rn scrub2: W=$wb/$wn target_parity=$tp"
		done < /tmp/fst.rows
		log "FSR rows=$rows acked=$acked stale_after1=$stale1 parity_kept=$kept recorded=${rec0:-?}/${rec1:-?}/${rec2:-?} replace_rc=$rrc uncorr=${unc:-?} uncopyable=${unco:-?} lost_msgs=$nlost ok=$ok eio=$eio wrong=$wrong changed=$changed"
		echo "$rows $acked $stale1 $kept ${rec0:-0} ${rec1:-0} $rrc ${unc:-0} ${unco:-0} $nlost $ok $eio $wrong $changed ${rec2:-0}" > $T/umltest/fsr.$TAG
		dmsetup remove_all 2>/dev/null
		finish
	fi
	# What a reader gets now that every device reads: the record, if it was
	# kept, makes the read rebuild the block from the parity.
	echo 3 > /proc/sys/vm/drop_caches
	while read -r _ n fo pw pv; do
		grep -qx $n $T/umltest/fst.acked.$TAG || continue
		echo "$n $(blk if=$MNT/nocow skip=$((fo / 4096)))"
	done < /tmp/fst.rows > /tmp/fst.read
	# A second scrub, every device healthy: the record, if it survived,
	# now gets its rebuild onto W's platter.
	btrfs scrub start -B $MNT > /tmp/fst.scrub2 2>&1
	log "scrub 2 rc=$?: $(grep -E 'Error summary|ERROR' /tmp/fst.scrub2 | tr '\n' ' ' | cut -c1-240)"
	rec2=$(hv recorded_blocks)
	log "after scrub 2: stale_marks=$(hv stale_marks) recorded_blocks=$rec2"
	umount $MNT || log "UMOUNT_FAIL"
	rows=0; acked=0; stale1=0; kept=0; lost=0; read_ok=0; repaired=0
	while read -r _ n fo pw pv; do
		rows=$((rows+1))
		grep -qx $n $T/umltest/fst.acked.$TAG || { log "FST row $n refused"; continue; }
		acked=$((acked+1))
		w1=$(awk -v n=$n '$1 == n {print $2}' /tmp/fst.plat1)
		p1=$(awk -v n=$n '$1 == n {print $3}' /tmp/fst.plat1)
		rd=$(awk -v n=$n '$1 == n {print $2}' /tmp/fst.read)
		w2=$(blk if=/dev/mapper/d$W iflag=direct skip=$((pw / 4096)))
		[ "$w1" = A ] && stale1=$((stale1+1))
		[ "$p1" = B ] && kept=$((kept+1))
		[ "$p1" = A ] && lost=$((lost+1))
		[ "$rd" = B ] && read_ok=$((read_ok+1))
		[ "$w2" = B ] && repaired=$((repaired+1))
		log "FST row $n rot=$((n % 4)) scrub1: W=$w1 parity=$p1 read=$rd scrub2: W=$w2"
	done < /tmp/fst.rows
	log "FST rows=$rows acked=$acked stale_after1=$stale1 parity_kept=$kept parity_lost=$lost read_ok=$read_ok repaired_after2=$repaired recorded=${rec0:-?}/${rec1:-?}/${rec2:-?} refusals=$nkept"
	echo "$rows $acked $stale1 $kept $lost $read_ok $repaired ${rec0:-0} ${rec1:-0} ${rec2:-0} $nkept" > $T/umltest/fst.$TAG
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
mixed_unchecked)
	# Data without checksums in a MIXED (data+metadata) RAID6 block group,
	# read back through a rebuild nothing can check.  The RAID56 layer
	# cannot look up checksums for a mixed block group, and used to count
	# every sector of one as checked -- as if a checksum above it would
	# reject a wrong rebuild -- so nodatasum data there came back WRONG
	# with no error.  Now the reader's inode decides.  --mixed, RAID6 over
	# five devices (PROFILE=raid6:raid6).
	#   SUB=parity  nodatasum file.  Q of one row overwritten on disk, the
	#               device of that row's data column gone.  Mirror 2 (from
	#               P, checked against Q) fails; mirror 3 rebuilt from Q
	#               alone and returned it.  Must fail with EIO and a
	#               read_parities_disagree alert.
	#   SUB=stale   nodatacow file.  A write into column B fails -- only
	#               that column's 64 KiB, so no metadata stripe is left
	#               stale -- then C's and P's devices are gone: C's rebuild
	#               folds in B's stale content.  Must fail with EIO and a
	#               read_unverifiable alert.
	#   SUB=reloc   as parity, but mounted read-write and degraded, and the
	#               missing device removed ('btrfs device remove missing'):
	#               relocation reads the block through the same rebuilds,
	#               for the data relocation inode.  Must fail the removal,
	#               with a read_parities_disagree alert, rather than copy
	#               mirror 3's rebuild into the new chunk.
	# CONTROL=1 sets raid56_read_trusts_ambiguous=1, which for a mixed
	# block group is exactly the old behaviour: wrong data, no error.  For
	# SUB=reloc it sets raid56_read_trusts_mixed_reloc=1 instead, the old
	# answer for relocation alone: the removal finishes, and the block reads
	# back wrong from the new chunk with every device present.
	SUB=${SUB:-parity}
	R=$T/umltest/mixu.$TAG; rm -f $R
	if [ "${CONTROL:-0}" = 1 ]; then
		knob=raid56_read_trusts_ambiguous
		[ "$SUB" = reloc ] && knob=raid56_read_trusts_mixed_reloc
		echo 1 > /sys/module/btrfs/parameters/$knob 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: $knob=1"
	fi
	watchdog ${WATCH:-600}
	dm_setup
	mkfs.btrfs -K -q -f --mixed -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	if [ "$SUB" = stale ]; then
		do_mount $OPTS /dev/mapper/d0
		allow_nodatacow
		# B's stripe must still be recorded stale when it is read.
		echo 600000 > /sys/module/btrfs/parameters/raid56_repair_delay_ms
		touch $MNT/f; chattr +C $MNT/f || log "CHATTR_FAIL"
	else
		do_mount $OPTS,nodatasum /dev/mapper/d0
	fi
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	mixed=0
	btrfs filesystem df $MNT 2>/dev/null | grep -qi '^Data+Metadata, RAID6' && mixed=1
	log "mixed RAID6 block group: $mixed ($(btrfs filesystem df $MNT 2>&1 | tr '\n' '|'))"
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/f
	sync
	L=$(python3 $T/umltest/raid56_layout.py $MNT/f /dev/mapper/d0 2>&1)
	log "layout: $L"
	eval "$L"
	[ -n "${IDX_Q:-}" ] || { log "LAYOUT_FAIL"; finish; }
	acked=1
	if [ "$SUB" = stale ]; then
		# The "error" target fails reads and writes of its range and
		# takes no flushes, so the device's cache flushes still work
		# and nothing but this column is recorded against it.
		d=$(echo $DEVS | awk -v i=$((IDX_B + 1)) '{print $i}')
		sz=$(blockdev --getsz $d)
		s=$((PHYS_B / 512))
		printf '0 %d linear %s 0\n%d 128 error\n%d %d linear %s %d\n' \
			$s $d $s $((s + 128)) $((sz - s - 128)) $d $((s + 128)) |
			{ dmsetup suspend --nolockfs --noflush d$IDX_B && dmsetup reload d$IDX_B &&
			  dmsetup resume d$IDX_B; } || log "DM_RELOAD_FAIL d$IDX_B"
		log "d$IDX_B fails I/O to column B of full stripe $FULL only (sectors $s+128)"
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' |
			dd of=$MNT/f bs=4096 seek=$((FO_B / 4096)) count=1 conv=notrunc,fsync \
			status=none 2>/dev/null && log "B acknowledged" || { log "B refused"; acked=0; }
		dm_heal $IDX_B
		sync
		log "HEALTH[written] $(tr '\n' '|' < $H)"
		umount $MNT || log "UMOUNT_FAIL"
		GONE="$IDX_C $IDX_P"
		TGT=$FO_C
		SANE=$((FO_B + NDATA * 65536))	# column 0 of the next full stripe
		KEY=read_unverifiable
	else
		umount $MNT || log "UMOUNT_FAIL"
		# Row 0 of Q only.  For three data columns of 'A', Q is 0xda in
		# every byte, so 'Z' contradicts P there and nowhere else.
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'Z' |
			dd of=/dev/mapper/d$IDX_Q bs=4096 seek=$((PHYS_Q / 4096)) count=1 \
			conv=notrunc,fsync status=none || log "CORRUPT_FAIL"
		log "Q of row 0 of full stripe $FULL overwritten on d$IDX_Q at $PHYS_Q"
		GONE=$IDX_B
		TGT=$FO_B
		SANE=$((FO_B + 4096))		# row 1 of the same column: P and Q agree
		KEY=read_parities_disagree
	fi
	# Known missing at mount, and ro so that mount recovery does not run.
	for g in $GONE; do dmsetup remove d$g || log "REMOVE_FAIL d$g"; done
	btrfs device scan --forget >/dev/null 2>&1
	m=
	for i in $(seq 0 $((NDEV-1))); do
		case " $GONE " in *" $i "*) continue;; esac
		btrfs device scan /dev/mapper/d$i >/dev/null 2>&1
		[ -n "$m" ] || m=$i
	done
	rmrc=
	if [ "$SUB" = reloc ]; then
		do_mount rw,degraded,nodatasum /dev/mapper/d$m
		btrfs device remove missing $MNT > /tmp/mixu.rm 2>&1
		rmrc=$?
		log "device remove missing rc=$rmrc: $(tr '\n' ' ' < /tmp/mixu.rm | cut -c1-160)"
		log "after it: $(btrfs filesystem show $MNT 2>&1 | grep -c devid) device(s), $(btrfs filesystem df $MNT 2>&1 | tr '\n' '|' | cut -c1-160)"
	else
		do_mount ro,degraded /dev/mapper/d$m
	fi
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	echo 3 > /proc/sys/vm/drop_caches
	rd() {	# offset: "<rc> <bytes of 'A'>" of one 4K block, O_DIRECT
		local rc
		rm -f /tmp/mixu.blk
		dd if=$MNT/f of=/tmp/mixu.blk bs=4096 skip=$(($1 / 4096)) count=1 \
			iflag=direct status=none 2>/dev/null
		rc=$?
		[ "$(stat -c %s /tmp/mixu.blk 2>/dev/null)" = 4096 ] || rc=1
		echo "$rc $(tr -cd 'A' < /tmp/mixu.blk 2>/dev/null | wc -c)"
	}
	read -r s_rc s_a <<< "$(rd $SANE)"
	read -r t_rc t_a <<< "$(rd $TGT)"
	n=$(sed -n "s/^$KEY //p" $H)
	ref=0
	dmesg | grep -q "REFUSED a read of full stripe" && ref=1
	log "HEALTH[after] $(tr '\n' '|' < $H)"
	kmsg "raid56:|Q syndrome" 8
	log "MIXU sub=$SUB mixed=$mixed acked=$acked sane=$s_rc/$s_a target=$t_rc/$t_a $KEY=${n:-?} refused_msg=$ref${rmrc:+ remove_rc=$rmrc}"
	echo "$mixed $acked $s_rc $s_a $t_rc $t_a ${n:-0} $ref $rmrc" > $R
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
	knob=/sys/module/btrfs/parameters/raid56_wf_name_unwritten
	log "knob raid56_wf_name_unwritten=$(cat $knob 2>/dev/null || echo absent)"
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
	# Whether the read failed, apart from what it returned: a refusal and
	# a read of something else both hold no 'B'.
	rm -f $T/umltest/blk.$TAG
	if dd if=$MNT/nocow of=$T/umltest/blk.$TAG bs=4096 skip=$((FO_B / 4096)) count=1 \
	      status=none 2>/dev/null; then
		rc=0
	else
		rc=1
	fi
	got=$(tr -cd 'B' 2>/dev/null < $T/umltest/blk.$TAG | wc -c)
	rm -f $T/umltest/blk.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	plat=$(dd if=$DEV_B bs=4096 skip=$((PHYS_B / 4096)) count=1 status=none 2>/dev/null |
	       tr -cd 'B' | wc -c)
	log "RMW_CACHE b_read=$got b_platter=$plat read_failed=$rc"
	echo "$got $plat $rc" > $T/umltest/rmw.cache.$TAG
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
replace_whole_column)
	# Does a RAID5 device replace give the new device the whole column of
	# every full stripe it copies, or only the sectors an extent covers?
	#
	# RAID5 data over three devices (two data columns a full stripe), RAID1
	# metadata, the fourth device the replace target.  A directory of
	# nodatacow files of 64 KiB each -- one data column -- written one after
	# the other, so they fill the chunk a column at a time; then every
	# second one is deleted.  Each full stripe now holds one column of data
	# and one free one, and its parity is still the one computed with the
	# deleted files' content in the free column.  'btrfs replace start -K'
	# swaps the second device for the target (no TRIM: the target keeps its
	# zeros wherever nothing is written).  Then each of the other two
	# devices in turn is left out of a read-only degraded mount, and every
	# surviving file is read: wherever the one left out held a file and the
	# replaced device's column of that full stripe was free, the file is
	# rebuilt from the parity and the TARGET's copy of the free column.
	#
	# Fixed, the target got the free columns too, rebuilt from the parity
	# and the other column, and every file reads back as written.
	# CONTROL=1 sets raid56_wf_replace_extents_only=1: the free columns
	# never reach the target, which holds zeros there instead of what the
	# parity was computed with, and the files rebuilt against them come back
	# wrong, with no error (no checksum).
	watchdog ${WATCH:-900}
	dm_setup
	set -- $DMDEVS
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $1 $2 $3 || { log "MKFS_FAIL"; finish; }
	TGT=$4
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 >/dev/null 2>&1
	do_mount $OPTS $1
	allow_nodatacow
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_replace_extents_only 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: the replace copies extents only"
	}
	mkdir -p $MNT/f; chattr +C $MNT/f || { log "CHATTR_FAIL"; finish; }
	NF=${NF:-192}
	for i in $(seq 0 $((NF - 1))); do
		dd if=/dev/urandom of=$MNT/f/$i bs=64K count=1 oflag=direct status=none ||
			log "WRITE_FAIL $i"
	done
	sync
	for i in $(seq 1 2 $((NF - 1))); do rm -f $MNT/f/$i; done
	sync; btrfs filesystem sync $MNT
	for i in $(seq 0 2 $((NF - 1))); do
		echo "$i $(md5sum < $MNT/f/$i | awk '{print $1}')"
	done > /tmp/rwc.md5
	filefrag -v $MNT/f/0 $MNT/f/2 2>/dev/null | grep -E '^ +0:' | while read -r l; do log "extent: $l"; done
	btrfs replace start -B -K -f $2 $TGT $MNT > /tmp/rep.out 2>&1
	rrc=$?
	log "replace rc=$rrc: $(tr '\n' ' ' < /tmp/rep.out | cut -c1-200)"
	st=$(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ')
	log "replace status: $(echo "$st" | cut -c1-160)"
	unc=$(echo "$st" | grep -o '[0-9]* uncorr' | awk '{print $1}')
	kmsg "replace|could neither copy" 6
	umount $MNT || log "UMOUNT_FAIL"
	# Leave each surviving original device out in turn.  Read-only, so the
	# one left out is still current when it is scanned back in.
	ok=0; wrong=0; eio=0
	for X in $1 $3; do
		keep=$(echo $1 $3 $TGT | tr ' ' '\n' | grep -vx "$X" | tr '\n' ' ')
		btrfs device scan --forget >/dev/null 2>&1
		btrfs device scan $keep >/dev/null 2>&1
		if ! mount -o ro,degraded $(echo $keep | awk '{print $1}') $MNT; then
			log "MOUNT_FAIL(ro,degraded without $X)"; kmsg "BTRFS" 4; continue
		fi
		o=0; w=0; e=0
		while read -r i m; do
			got=$(md5sum < $MNT/f/$i 2>/dev/null | awk '{print $1}')
			if [ -z "$got" ]; then e=$((e + 1))
			elif [ "$got" = "$m" ]; then o=$((o + 1))
			else w=$((w + 1)); log "WRONG f/$i without $X"
			fi
		done < /tmp/rwc.md5
		log "without $X: ok=$o wrong=$w eio=$e"
		ok=$((ok + o)); wrong=$((wrong + w)); eio=$((eio + e))
		umount $MNT || log "UMOUNT_FAIL"
	done
	log "RWC rrc=$rrc unc=${unc:-?} ok=$ok wrong=$wrong eio=$eio"
	echo "$rrc ${unc:-?} $ok $wrong $eio" > $T/umltest/rwc.$TAG
	dmsetup remove_all 2>/dev/null
	finish
	;;
replace_torn_free)
	# A crash tore a RAID5 full stripe, and then the device of its free
	# data column is gone: does the replace of that device put the rebuild
	# of the column on the new device, or count its free sectors as lost?
	#
	# RAID5 data, RAID1C3 metadata over four devices, a fifth the replace
	# target.  PHASE=1, every device present: a 128 KiB nodatacow file of
	# 'A', the first data written, fills columns 0 and 1 of the first full
	# stripe; column 2, on device X, holds no extent.  Block 16 (column 1,
	# row 0) is overwritten in place with 'C' with raid56_crash_point=1
	# armed: the 'C's land, the parity does not, and the kernel panics.
	# PHASE=2: device X is left out, the array mounted degraded read-write.
	# The recovery meets the stripe in flight at the crash, with a column on
	# the missing device: no data without a checksum there, so it is
	# decided and kept, marked possibly torn (wib_keep_torn()).  Then
	# 'btrfs replace start X <target>'.
	#
	# Fixed, the replace rebuilds column 2 -- nothing but free sectors --
	# onto the target: row 0 there is 'A' ^ 'C', the value that makes the
	# row agree with its parity, and nothing is counted lost, alerted
	# (replace_uncopyable, read_unrecovered) or marked stale.  CONTROL=1
	# sets raid56_wf_replace_refuses_free=1: the rebuild is refused as data
	# rebuilt from a torn parity, every free sector of the column counts as
	# lost, and the target holds zeros there, recorded stale.
	watchdog ${WATCH:-900}
	dm_setup
	set -- $DMDEVS
	TGT=$5
	if [ "${PHASE:-1}" = 1 ]; then
		mkfs.btrfs -K -q -f -d raid5 -m raid1c3 $1 $2 $3 $4 || { log "MKFS_FAIL"; finish; }
		btrfs device scan --forget >/dev/null 2>&1
		btrfs device scan $1 $2 $3 $4 >/dev/null 2>&1
		do_mount $OPTS $1
		allow_nodatacow
		touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
		dd if=/dev/zero bs=64K count=2 status=none | tr '\000' 'A' > $MNT/nocow
		sync
		python3 $T/umltest/raid56_rows.py $MNT/nocow $1 1 32 phys \
			> $T/umltest/rtf.rows.$TAG 2>&1
		col() { awk -v b=$1 '$2 == b {print $3}' $T/umltest/rtf.rows.$TAG; }
		# Column 2 of block 0's row: "<file block>:<device>:<offset>".
		c2=$(awk '$2 == 0 {print $7}' $T/umltest/rtf.rows.$TAG)
		if [ "$(col 0) $(col 15) $(col 16) $(col 31)" != "0 0 1 1" ] ||
		   [ "${c2%%:*}" != -1 ]; then
			log "LAYOUT_FAIL: $(head -1 $T/umltest/rtf.rows.$TAG)"
			finish
		fi
		r=${c2#*:}
		echo "X=${r%%:*} PHYS_X=${r#*:}" > $T/umltest/rtf.lay.$TAG
		log "layout: column 2 of the full stripe is free, on $(cat $T/umltest/rtf.lay.$TAG);" \
		    "$(head -1 $T/umltest/rtf.rows.$TAG)"
		echo 1 > /sys/module/btrfs/parameters/raid56_crash_point || log "CRASH_ARM_FAIL"
		log "crash armed"
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'C' |
			dd of=$MNT/nocow bs=4096 seek=16 count=1 oflag=direct conv=notrunc \
			status=none 2>/dev/null
		log "NO_CRASH"
		finish
	fi
	eval "$(cat $T/umltest/rtf.lay.$TAG)"
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_replace_refuses_free 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: raid56_wf_replace_refuses_free=1"
	}
	dmsetup remove d$X || log "DM_REMOVE_FAIL"
	keep=""
	for d in 0 1 2 3; do [ $d = $X ] || keep="$keep /dev/mapper/d$d"; done
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $keep >/dev/null 2>&1
	do_mount $OPTS,degraded $(echo $keep | awk '{print $1}')
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	W=$(ls /sys/fs/btrfs/*-*-*/raid56_write_intent 2>/dev/null | head -1)
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	wv() { awk -v k=$1 '$1 == k {print $2}' $W 2>/dev/null; }
	kmsg "write-intent|scrub: full stripe" 6
	log "RTF after the recovery: torn_blocks=$(wv torn_blocks)" \
	    "sticky_blocks=$(wv sticky_blocks) stale_marks=$(hv stale_marks)"
	btrfs replace start -B -K -f $((X + 1)) $TGT $MNT > $T/umltest/rtf.out.$TAG 2>&1
	rrc=$?
	log "replace rc=$rrc: $(tr '\n' ' ' < $T/umltest/rtf.out.$TAG | cut -c1-200)"
	st=$(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ')
	log "replace status: $(echo "$st" | cut -c1-160)"
	unc=$(echo "$st" | grep -o '[0-9]* uncorr' | awk '{print $1}')
	# What the target holds in row 0 of column 2: bytes that are not zero.
	nz=$(dd if=$TGT bs=4096 skip=$((PHYS_X / 4096)) count=1 iflag=direct status=none |
	     tr -d '\000' | wc -c)
	echo 3 > /proc/sys/vm/drop_caches
	ok=0; bad=0
	for b in $(seq 0 31); do
		n=$(dd if=$MNT/nocow bs=4096 skip=$b count=1 status=none 2>/dev/null |
		    tr -cd 'AC' | wc -c)
		if [ "$n" = 4096 ]; then ok=$((ok + 1)); else bad=$((bad + 1)); fi
	done
	kmsg "replace|could neither copy|raid56:" 8
	log "RTF rrc=$rrc unc=${unc:-?} uncopyable=$(hv replace_uncopyable)" \
	    "read_unrecovered=$(hv read_unrecovered) stale_marks=$(hv stale_marks)" \
	    "target_row0_nonzero=$nz file_ok=$ok file_bad=$bad state=$(hv state)"
	echo "$rrc ${unc:-?} $(hv replace_uncopyable) $(hv read_unrecovered)" \
	     "$(hv stale_marks) $nz $ok $bad" > $T/umltest/rtf.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
replace_uncopyable)
	# A sector a RAID5 device replace can neither copy nor rebuild: what
	# does the new device hold there, and what does a read of it return?
	#
	# RAID5 data over three devices, RAID1 metadata, the fourth device the
	# replace target.  One 2 MiB nodatacow file of 'A'; raid56_layout.py
	# finds a full stripe inside it, whose column 0 is on SRC and column 1
	# on SIB.  The first 4 KiB of each -- the same row -- fails every IO:
	# SRC cannot return its sector, and the parity rebuild of it needs
	# SIB's.  Then 'btrfs replace start -K SRC' (no TRIM: the target holds
	# zeros wherever nothing is written), and that block of the file is
	# read, first with SIB's sector still failing, then healed.
	#
	# Fixed, the replace finishes and counts one uncorrectable read error,
	# the target holds zeros there and the write-intent log records its
	# column stale: the read fails with EIO while SIB's sector does, and
	# returns 'A', rebuilt from the parity, once it is healed.  CONTROL=1
	# sets raid56_wf_replace_extents_only=1: the replace counts nothing, the
	# sector never reaches the target, and both reads return its zeros
	# without an error.
	watchdog ${WATCH:-900}
	dm_setup
	set -- $DMDEVS
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $1 $2 $3 || { log "MKFS_FAIL"; finish; }
	TGT=$4
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 >/dev/null 2>&1
	do_mount $OPTS $1
	allow_nodatacow
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_replace_extents_only 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: the replace copies extents only"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	want=$(md5sum < $MNT/nocow | awk '{print $1}')
	L=$(python3 $T/umltest/raid56_layout.py $MNT/nocow $1 2>&1)
	log "layout: $L"
	eval "$L"
	[ -n "${FO_B:-}" ] && [ -n "${PHYS_C:-}" ] || { log "LAYOUT_FAIL"; finish; }
	# One 4 KiB sector of a device fails every IO; the rest maps through.
	bad_sector() {	# index physical-offset
		local d=$(echo $DEVS | awk -v i=$(( $1 + 1 )) '{print $i}')
		local sz=$(blockdev --getsz $d) s=$(( $2 / 512 ))
		printf '0 %d linear %s 0\n%d 8 error\n%d %d linear %s %d\n' \
			$s $d $s $((s + 8)) $((sz - s - 8)) $d $((s + 8)) |
			{ dmsetup suspend --nolockfs --noflush d$1 && dmsetup reload d$1 &&
			  dmsetup resume d$1; } || log "DM_RELOAD_FAIL d$1"
	}
	bad_sector $IDX_B $PHYS_B
	bad_sector $IDX_C $PHYS_C
	log "unreadable: d$IDX_B at $PHYS_B (source), d$IDX_C at $PHYS_C (sibling)"
	btrfs replace start -B -K -f $DEV_B $TGT $MNT > /tmp/rep.out 2>&1
	rrc=$?
	log "replace rc=$rrc: $(tr '\n' ' ' < /tmp/rep.out | cut -c1-200)"
	st=$(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ')
	log "replace status: $(echo "$st" | cut -c1-160)"
	unc=$(echo "$st" | grep -o '[0-9]* uncorr' | awk '{print $1}')
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	log "health: state=$(hv state) replace_uncopyable=$(hv replace_uncopyable) stale_marks=$(hv stale_marks)"
	# RUA=1 (replace_abort.sh): how raid56_health reads once the alert
	# work has run -- at once, for the first event of a kind -- and after
	# 'echo ack'.
	if [ "${RUA:-0}" = 1 ]; then
		ua() {
			awk '$1 == "unacknowledged" {
				$1 = ""; sub(/^ /, ""); gsub(/ /, ","); print }' $H
		}
		sleep 3
		st1=$(hv state) ua1=$(ua)
		echo ack > $H 2>/dev/null || log "ACK_FAIL"
		sleep 1
		st2=$(hv state) ua2=$(ua)
		log "RUA state=$st1 unacknowledged=$ua1, after ack state=$st2" \
		    "unacknowledged=$ua2, replace_aborted=$(hv replace_aborted)"
		kmsg "replace was ABORTED|cannot record them" 4
		echo "$rrc ${st1:-?} ${ua1:-?} ${st2:-?} ${ua2:-?} $(hv replace_aborted)" \
			> $T/umltest/rua.$TAG
	fi
	blk() {	# echoes "<bytes read> <bytes of 'A'>"
		echo 3 > /proc/sys/vm/drop_caches
		local n a
		n=$(dd if=$MNT/nocow bs=4096 skip=$((FO_B / 4096)) count=1 status=none 2>/dev/null | wc -c)
		a=$(dd if=$MNT/nocow bs=4096 skip=$((FO_B / 4096)) count=1 status=none 2>/dev/null |
		    tr -cd 'A' | wc -c)
		echo "$n $a"
	}
	read -r n1 a1 <<<"$(blk)"
	log "with the sibling's sector failing: read $n1 bytes, $a1 of them 'A'"
	dm_heal $IDX_C
	read -r n2 a2 <<<"$(blk)"
	log "sibling healed: read $n2 bytes, $a2 of them 'A'"
	got=$(md5sum < $MNT/nocow 2>/dev/null | awk '{print $1}')
	md5ok=0; [ "$got" = "$want" ] && md5ok=1
	kmsg "replace|could neither copy|raid56:" 8
	log "RUC rrc=$rrc unc=${unc:-?} n1=$n1 a1=$a1 n2=$n2 a2=$a2 md5ok=$md5ok uncopyable=$(hv replace_uncopyable)"
	echo "$rrc ${unc:-?} $n1 $a1 $n2 $a2 $md5ok $(hv replace_uncopyable)" > $T/umltest/ruc.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
replace_source_data)
	# Does a RAID5 device replace copy data without a checksum from the old
	# device, which is what every read returned until now, or put on the
	# new one a rebuild from the parity that nothing checks?
	#
	# RAID5 data over three devices, RAID1 metadata, the fourth device the
	# replace target.  One 2 MiB nodatacow file of 'A'; raid56_layout.py
	# finds a full stripe inside it, whose column 0 is on SRC.  With the
	# filesystem unmounted, the parity of that stripe's rows 0 and 3 is
	# overwritten with 'Z': silent rot, which the write-intent log knows
	# nothing of and no read of the file has needed.  UNREAD=1 also makes
	# SRC's row 8 fail every IO: the scrub's read of the column then fails
	# as a whole, and the repair worker leaves an unverifiable rebuild of
	# all of it in the buffer, rotten rows included.  Then 'btrfs replace
	# start -K SRC', and rows 0, 3 and 8 of the column are read back from
	# the new device.
	#
	# Fixed, the replace copies what SRC returns -- with UNREAD=1 after
	# asking for it again sector by sector, all but row 8, which is rebuilt
	# from a parity that is right there -- and every row reads back as 'A'.
	# CONTROL=1 sets raid56_wf_replace_rebuilds_unchecked=1: the replace
	# puts the rebuild ('Z' ^ 'A') on the target, rows 0 and 3 read back as
	# that without an error, and the stripe now agrees with its parity, so
	# no scrub can find it again.
	watchdog ${WATCH:-900}
	dm_setup
	set -- $DMDEVS
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $1 $2 $3 || { log "MKFS_FAIL"; finish; }
	TGT=$4
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 >/dev/null 2>&1
	do_mount $OPTS $1
	allow_nodatacow
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	want=$(md5sum < $MNT/nocow | awk '{print $1}')
	L=$(python3 $T/umltest/raid56_layout.py $MNT/nocow $1 2>&1)
	log "layout: $L"
	eval "$L"
	[ -n "${FO_B:-}" ] && [ -n "${PHYS_P:-}" ] || { log "LAYOUT_FAIL"; finish; }
	umount $MNT || log "UMOUNT_FAIL"
	for r in 0 3; do
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'Z' |
			dd of=$DEV_P bs=4096 seek=$(( PHYS_P / 4096 + r )) count=1 \
			   oflag=direct conv=notrunc status=none || log "ROT_FAIL row $r"
	done
	log "parity rows 0 and 3 of full stripe $FULL rotten on $DEV_P"
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 >/dev/null 2>&1
	do_mount $OPTS $1
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_replace_rebuilds_unchecked 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: the replace rebuilds data without a checksum"
	}
	if [ "${UNREAD:-0}" = 1 ]; then
		dm_bad_sectors $IDX_B $(( PHYS_B + 8 * 4096 ))
		log "unreadable: d$IDX_B at $(( PHYS_B + 8 * 4096 )) (row 8 of the source's column)"
	fi
	read -r pn0 pa0 <<<"$(nocow_blk $FO_B)"
	log "before the replace: row 0 reads $pn0 bytes, $pa0 of them 'A'"
	btrfs replace start -B -K -f $DEV_B $TGT $MNT > /tmp/rep.out 2>&1
	rrc=$?
	log "replace rc=$rrc: $(tr '\n' ' ' < /tmp/rep.out | cut -c1-200)"
	log "replace status: $(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ' | cut -c1-160)"
	read -r n0 a0 <<<"$(nocow_blk $FO_B)"
	read -r n3 a3 <<<"$(nocow_blk $(( FO_B + 3 * 4096 )))"
	read -r n8 a8 <<<"$(nocow_blk $(( FO_B + 8 * 4096 )))"
	log "after: row 0 $n0 bytes ($a0 'A'), row 3 $n3 bytes ($a3 'A'), row 8 $n8 bytes ($a8 'A')"
	echo 3 > /proc/sys/vm/drop_caches
	got=$(md5sum < $MNT/nocow 2>/dev/null | awk '{print $1}')
	md5ok=0; [ "$got" = "$want" ] && md5ok=1
	kmsg "replace|rebuilt from the parity|raid56:" 8
	log "RSD rrc=$rrc pre=$pa0 n0=$n0 a0=$a0 n3=$n3 a3=$a3 n8=$n8 a8=$a8 md5ok=$md5ok"
	echo "$rrc $pa0 $n0 $a0 $n3 $a3 $n8 $a8 $md5ok" > $T/umltest/rsd.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
replace_abort_marks)
	# What does a RAID5 device replace that does not finish leave behind of
	# the stale mark it made for a sector it could neither copy nor rebuild?
	#
	# RAID5 data over three devices, RAID1 metadata, the fourth device the
	# replace target.  One 2 MiB nodatacow file of 'A'; raid56_layout.py
	# finds a full stripe inside it, column 0 on SRC and column 1 on SIB.
	# Row 0 of both fails every IO, so the replace can neither copy nor
	# rebuild SRC's row 0, and records SRC's column stale for the zeros it
	# puts on the target there.  Row 5 of SIB fails too: row 5 of SRC's
	# column cannot be rebuilt, but SRC returns it.  The target fails every
	# write to the next two data columns SRC holds (full stripes +2 and +3,
	# dm-flakey), so the replace aborts at the end of the chunk, after that
	# column is copied, and SRC stays in the filesystem.  Then row 5 of
	# SRC's column is read with SIB's row 5 still failing, and
	# raid56_health's stale_marks.
	#
	# Fixed, the abort takes the mark back with the target: the read returns
	# SRC's 'A' as it did before the replace, and no stale mark is left.
	# CONTROL=1 sets raid56_wf_replace_keeps_marks=1: the mark stays on
	# SRC's column, the read is forced through a rebuild that SIB's row 5
	# cannot give, and fails with EIO although SRC holds the right bytes.
	watchdog ${WATCH:-900}
	dm_setup
	set -- $DMDEVS
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $1 $2 $3 || { log "MKFS_FAIL"; finish; }
	TGT=$4
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 >/dev/null 2>&1
	do_mount $OPTS $1
	allow_nodatacow
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_replace_keeps_marks 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: the replace's stale marks apply at once and outlive it"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	want=$(md5sum < $MNT/nocow | awk '{print $1}')
	L=$(python3 $T/umltest/raid56_layout.py $MNT/nocow $1 2>&1)
	log "layout: $L"
	eval "$L"
	[ -n "${FO_B:-}" ] && [ -n "${PHYS_C:-}" ] || { log "LAYOUT_FAIL"; finish; }
	# SRC's pieces of full stripes +2 and +3 are data columns (+1 is P);
	# the target gets the same physical layout.  Keep clear of the
	# superblock copy at 64 MiB, which the replace writes too.
	W0=$(( PHYS_B + 2 * 65536 )); WLEN=$(( 2 * 65536 ))
	if [ $W0 -lt $(( 67108864 + 4096 )) ] && [ $(( W0 + WLEN )) -gt 67108864 ]; then
		log "LAYOUT_FAIL: the superblock copy at 64 MiB lies in the target's failing range"
		finish
	fi
	dm_bad_sectors $IDX_B $PHYS_B
	dm_bad_sectors $IDX_C $PHYS_C $(( PHYS_C + 5 * 4096 ))
	dm_error_writes_range 3 $W0 $WLEN
	log "unreadable: d$IDX_B at $PHYS_B (source row 0), d$IDX_C at $PHYS_C and $(( PHYS_C + 5 * 4096 )) (sibling rows 0 and 5); target fails writes at [$W0, $(( W0 + WLEN )))"
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	read -r pn5 pa5 <<<"$(nocow_blk $(( FO_B + 5 * 4096 )))"
	log "before the replace: row 5 reads $pn5 bytes, $pa5 of them 'A'"
	btrfs replace start -B -K -f $DEV_B $TGT $MNT > /tmp/rep.out 2>&1
	rrc=$?
	log "replace rc=$rrc: $(tr '\n' ' ' < /tmp/rep.out | cut -c1-200)"
	log "replace status: $(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ' | cut -c1-160)"
	inuse=0; btrfs filesystem show $MNT 2>/dev/null | grep -q "$DEV_B" && inuse=1
	marks=$(hv stale_marks)
	log "health: state=$(hv state) stale_marks=$marks recorded_blocks=$(hv recorded_blocks) replace_uncopyable=$(hv replace_uncopyable); source still a member: $inuse"
	read -r n5 a5 <<<"$(nocow_blk $(( FO_B + 5 * 4096 )))"
	read -r n0 a0 <<<"$(nocow_blk $FO_B)"
	log "after the abort: row 5 reads $n5 bytes ($a5 'A'), row 0 $n0 bytes"
	dm_heal $IDX_C
	echo 3 > /proc/sys/vm/drop_caches
	got=$(md5sum < $MNT/nocow 2>/dev/null | awk '{print $1}')
	md5ok=0; [ "$got" = "$want" ] && md5ok=1
	log "sibling healed: md5ok=$md5ok"
	kmsg "replace|could neither copy|did not finish|raid56:" 10
	log "RAM rrc=$rrc inuse=$inuse pre5=$pa5 n5=$n5 a5=$a5 n0=$n0 marks=${marks:-?} unc=$(hv replace_uncopyable) md5ok=$md5ok"
	echo "$rrc $inuse $pa5 $n5 $a5 $n0 ${marks:-?} $(hv replace_uncopyable) $md5ok" > $T/umltest/ram.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
replace_marks_kept)
	# Does a replace of a missing RAID5 device keep its record of the zeros
	# it puts on the new device where it can neither copy nor rebuild, in a
	# write-intent log that is full while a device is missing?
	#
	# RAID5 data, RAID1C3 metadata over four devices, a fifth the replace
	# target.  One nodatacow file of 'A', RM_REGIONS regions (4 MiB) long;
	# raid56_rows.py lists one row per region, with physical offsets.  Device
	# 3 (devid 4) is pulled and the array mounted degraded.  Then, by ARM:
	#   keep  in the first two rows the replace reaches (the lowest on device
	#         3) whose column there holds data, a sibling's sector fails
	#         every IO: the replace can neither copy nor rebuild those two
	#         sectors, puts zeros there and records them, the second record
	#         after the first.  Every other row with data on device 3 is
	#         written into first, in that column: each record names the
	#         missing column and none retires -- the replace rebuilds such a
	#         column onto its target and leaves the record, where it would
	#         retire one of a stripe whose parity it regenerates -- so the
	#         log fills and turns over (wib_may_evict_naming()).
	#   lost  device 0 fails every read over the file's rows: the replace can
	#         rebuild nothing of device 3, and has to record more stripes
	#         than the log holds; one record has to go.
	#   stale the same two rows as keep, but written into (degraded, 'B')
	#         before the replace, first: the log names their column stale
	#         already when the replace records the zeros it puts there, the
	#         usual case with the device missing, and their records take the
	#         first slots.  Then rows after them in logical order, until the
	#         log is exactly full (replace_marks_rows.py); a sibling's sector
	#         of the RM_LATE rows last on device 3 fails every IO too, and
	#         their records are the replace's own, each a region the full
	#         log has to spend a record for.
	#   resume the same as stale, but the late rows fail only after a crash
	#         (PHASE=1) that comes once the replace has recorded the first
	#         two and moved past their device extent.  PHASE=2 mounts
	#         degraded again: the log reloads their records as ordinary ones,
	#         in logical order, so first, and the replace resumes at mount.
	#   resume-lost  lost, but the machine crashes as soon as the replace
	#         runs (PHASE=1), and device 0 fails its reads only in PHASE=2,
	#         which mounts degraded again: the replace resumes at mount, in
	#         the kernel's own thread, and fails as lost's does.
	# Then 'btrfs replace start 4 <target>' (resume: resumed), and device 3's
	# sectors of the sampled rows (keep, stale, resume: the first two; lost:
	# every row) are read back, the failing sectors still failing:
	#   keep  fixed, the replace finishes and both rows read EIO (recorded);
	#         CONTROL=1 (raid56_wf_evict_replace_marks=1) the second record
	#         spent the first, and its row reads back as the target's zeros
	#   lost  fixed, the replace fails (replace_record_dropped), device 4
	#         stays missing and no row reads as zeros; CONTROL=1 the replace
	#         finishes, and rows whose records were spent read back as zeros
	#   resume-lost  fixed, the resumed replace fails with the alert and
	#         a message; CONTROL=1 (raid56_wf_resume_fail_warns=1) it fails
	#         with a kernel warning and backtrace as well (KERNEL_SPLAT)
	#   stale, resume  fixed, the replace finishes and both rows read EIO
	#         (resume: it copies again from the start, recording both
	#         again); CONTROL=1 (raid56_wf_replace_keeps_added_only=1) the
	#         records in the first slots are not the replace's -- it did not
	#         add the mark (stale), or made it in the mount before and
	#         resumes past it (resume) -- so the late rows' records spend
	#         them, and their rows read back as the target's zeros
	watchdog ${WATCH:-1500}
	dm_setup
	set -- $DMDEVS
	TGT=$5
	case "$ARM" in
	keep|lost) KNOB=raid56_wf_evict_replace_marks;;
	resume-lost) KNOB=raid56_wf_resume_fail_warns;;
	*) KNOB=raid56_wf_replace_keeps_added_only;;
	esac
	# Before the mount: a replace resumed at mount decides where from.
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/$KNOB 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: $KNOB=1"
	}
	ROWS=$T/umltest/rm.rows.$TAG
	# Make the sibling sectors of the rows listed in the files fail every IO.
	rm_bad() {
		local d o
		for d in 0 1 2; do
			o=$(cat "$@" | awk -v d=$d '$3 == d {print $4}' | tr '\n' ' ')
			[ -n "$o" ] && dm_bad_sectors $d $o
		done
		log "unreadable siblings:" "$(cat "$@" | awk '{print "d" $3 "@" $4}' | tr '\n' ' ')"
	}
	if [ "${PHASE:-1}" = 1 ]; then
		mkfs.btrfs -K -q -f -d raid5 -m raid1c3 $1 $2 $3 $4 || { log "MKFS_FAIL"; finish; }
		btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 $4 >/dev/null 2>&1
		do_mount $OPTS $1
		allow_nodatacow
		touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
		RM_REGIONS=${RM_REGIONS:-100}
		dd if=/dev/zero bs=1M count=$(( RM_REGIONS * 4 + 8 )) status=none | tr '\000' 'A' \
			> $MNT/nocow
		sync
		python3 $T/umltest/raid56_rows.py $MNT/nocow $1 1024 $RM_REGIONS phys > $ROWS 2>&1
		n=$(awk 'NF > 4 && $3 >= 0' $ROWS | wc -l)
		[ "$n" = "$RM_REGIONS" ] || {
			log "LAYOUT_FAIL $n of $RM_REGIONS rows: $(head -2 $ROWS | tr '\n' ' ')"
			finish
		}
		umount $MNT || log "UMOUNT_FAIL"
		# Device 3 goes missing.
		dmsetup remove d3 || log "DM_REMOVE_FAIL"
		btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 >/dev/null 2>&1
		do_mount $OPTS,degraded $1
	else
		# PHASE=2 of resume: device 3 still missing, the siblings still failing,
		# and the replace the crash interrupted resumes at mount.
		dmsetup remove d3 || log "DM_REMOVE_FAIL"
		if [ $ARM = resume-lost ]; then
			read -r lo len < $T/umltest/rm.d0range.$TAG
			dm_error_reads_range 0 $lo $len
			log "device 0 fails reads over [$lo, $(( lo + len )))"
		else
			rm_bad $T/umltest/rm.score.$TAG $T/umltest/rm.late.$TAG
		fi
		btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 $5 >/dev/null 2>&1
		do_mount $OPTS,degraded $1
	fi
	allow_nodatacow
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	W=$(ls /sys/fs/btrfs/*-*-*/raid56_write_intent 2>/dev/null | head -1)
	SPEED=$(dirname $H)/devinfo/4/scrub_speed_max
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	wv() { awk -v k=$1 '$1 == k {print $2}' $W 2>/dev/null; }
	# Tenths of a percent of device 3 the replace has copied, as far as the
	# cursor it keeps says: that moves one device extent at a time.
	progress() {
		btrfs replace status -1 $MNT 2>/dev/null |
			sed -n 's/^\([0-9]*\)\.\([0-9]\)% done.*/\1\2/p' | sed 's/^0*\(.\)/\1/'
	}
	if [ "${PHASE:-1}" = 1 ]; then
		# Device 3's data sector of each row:
		# "<row> <file block> <a sibling's device> <its offset> <device 3 offset>";
		# and where each row lies on device 3, data or parity: "<row> <offset>".
		: > $T/umltest/rm.pick.$TAG; : > $T/umltest/rm.d3.$TAG
		while read -r i fb col par mates; do
			tb=""; tp=""; sd=""; sp=""
			[ "${par%%:*}" = 3 ] && echo "$i ${par#*:}" >> $T/umltest/rm.d3.$TAG
			for m in $mates; do
				b=${m%%:*}; r=${m#*:}; d=${r%%:*}; p=${r#*:}
				[ "$b" -ge 0 ] 2>/dev/null || continue
				if [ "$d" = 3 ]; then tb=$b; tp=$p; elif [ -z "$sd" ]; then sd=$d; sp=$p; fi
			done
			[ -n "$tp" ] && echo "$i $tp" >> $T/umltest/rm.d3.$TAG
			[ -n "$tb" ] && [ -n "$sd" ] && echo "$i $tb $sd $sp $tp" >> $T/umltest/rm.pick.$TAG
		done < $ROWS
		case "$ARM" in
		keep|stale|resume)
			sort -n -k5 $T/umltest/rm.pick.$TAG | head -2 > $T/umltest/rm.score.$TAG
			[ $(wc -l < $T/umltest/rm.score.$TAG) = 2 ] || {
				log "LAYOUT_FAIL: fewer than two rows have data on device 3"
				finish
			}
			dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > $T/umltest/rm.bblock.$TAG
			if [ $ARM = keep ]; then
				# Rows the replace reaches before the second of those: left
				# alone.
				last=$(tail -1 $T/umltest/rm.score.$TAG | awk '{print $5}')
				skip=" $(awk -v l=$last '$2 <= l {print $1}' $T/umltest/rm.d3.$TAG |
					 tr '\n' ' ') "
				bad=0; nw=0
				while read -r i tb sd sp tp; do
					case "$skip" in *" $i "*) continue;; esac
					nw=$((nw + 1))
					dd if=$T/umltest/rm.bblock.$TAG of=$MNT/nocow bs=4096 oflag=direct \
					   conv=notrunc seek=$tb count=1 status=none 2>/dev/null ||
						bad=$((bad + 1))
				done < $T/umltest/rm.pick.$TAG
				sync
				log "RM degraded writes into device 3's column of $nw rows (not rows$skip):" \
				    "$bad failed, sticky_blocks=$(wv sticky_blocks)" \
				    "sticky_evicted=$(wv sticky_evicted)"
				rm_bad $T/umltest/rm.score.$TAG
			else
				# The two first, then rows after them in logical
				# order until the log is exactly full, their records
				# after the two's in the table; the last rows on
				# device 3 left for the replace to record.
				python3 $T/umltest/raid56_rows.py $MNT/nocow $1 1024 $RM_REGIONS \
					region > $T/umltest/rm.regs.$TAG 2>&1
				python3 $T/umltest/replace_marks_rows.py $T/umltest/rm.pick.$TAG \
					$T/umltest/rm.regs.$TAG ${RM_CAP:-82} ${RM_LATE:-4} \
					$T/umltest/rm.score.$TAG $T/umltest/rm.fill.$TAG \
					$T/umltest/rm.late.$TAG > $T/umltest/rm.lay.$TAG 2>&1 || {
					log "LAYOUT_FAIL: $(cat $T/umltest/rm.lay.$TAG)"
					finish
				}
				log "RM layout: $(cat $T/umltest/rm.lay.$TAG)"
				cat $T/umltest/rm.score.$TAG $T/umltest/rm.fill.$TAG \
					> $T/umltest/rm.wr.$TAG
				bad=0; nw=0
				while read -r i tb sd sp tp; do
					nw=$((nw + 1))
					dd if=$T/umltest/rm.bblock.$TAG of=$MNT/nocow bs=4096 oflag=direct \
					   conv=notrunc seek=$tb count=1 status=none 2>/dev/null ||
						bad=$((bad + 1))
				done < $T/umltest/rm.wr.$TAG
				sync
				log "RM degraded writes into device 3's column of $nw rows, the two first:" \
				    "$bad failed, stale_marks=$(hv stale_marks)" \
				    "sticky_evicted=$(wv sticky_evicted)"
				[ "$(wv sticky_evicted)" = 0 ] ||
					log "LAYOUT_FAIL: the log turned over before the replace"
				# resume: the late rows fail after the crash only.
				if [ $ARM = stale ]; then
					rm_bad $T/umltest/rm.score.$TAG $T/umltest/rm.late.$TAG
				else
					rm_bad $T/umltest/rm.score.$TAG
				fi
			fi
			;;
		lost|resume-lost)
			for m in $(awk '{for (f = 5; f <= NF; f++) print $f}' $ROWS); do
				r=${m#*:}; [ "${r%%:*}" = 0 ] && echo ${r#*:}
			done > $T/umltest/rm.d0.$TAG
			lo=$(sort -n $T/umltest/rm.d0.$TAG | head -1); hi=$(sort -n $T/umltest/rm.d0.$TAG | tail -1)
			echo "$lo $(( hi + 65536 - lo ))" > $T/umltest/rm.d0range.$TAG
			if [ $ARM = lost ]; then
				dm_error_reads_range 0 $lo $(( hi + 65536 - lo ))
				log "device 0 fails reads over [$lo, $(( hi + 65536 )))"
			fi
			cp $T/umltest/rm.pick.$TAG $T/umltest/rm.score.$TAG
			;;
		esac
		log "RM before the replace: stale_marks=$(hv stale_marks)" \
		    "recorded_blocks=$(hv recorded_blocks)" \
		    "sticky_blocks=$(wv sticky_blocks) inflight_blocks=$(wv inflight_blocks)" \
		    "sticky_evicted=$(wv sticky_evicted)"
	fi
	case "$ARM:${PHASE:-1}" in
	keep:*|lost:*|stale:*)
		btrfs replace start -B -K -f 4 $TGT $MNT > $T/umltest/rm.out.$TAG 2>&1
		rrc=$?
		log "replace rc=$rrc: $(tr '\n' ' ' < $T/umltest/rm.out.$TAG | cut -c1-200)"
		;;
	resume:1)
		echo ${RM_SPEED:-4194304} > $SPEED || log "THROTTLE_FAIL"
		rm -f $T/umltest/rm.rc.$TAG
		( btrfs replace start -B -K -f 4 $TGT $MNT > $T/umltest/rm.out.$TAG 2>&1
		  echo $? > $T/umltest/rm.rc.$TAG ) &
		# Both first rows recorded, and the cursor past the device
		# extent they are in.
		need=$(sort -n -k5 $T/umltest/rm.score.$TAG | tail -1 |
		       awk -v s=$(blockdev --getsize64 $1) '{print int($5 / int(s / 1000))}')
		n=0
		until [ "$(hv replace_uncopyable)" -ge 2 ] 2>/dev/null &&
		      [ "$(progress)" -gt $need ] 2>/dev/null; do
			[ -f $T/umltest/rm.rc.$TAG ] && break
			n=$((n + 1)); [ $n -gt 2400 ] && { log "RM_TIMEOUT"; break; }
			sleep 0.5
		done
		log "RM replace at $(progress)/1000 (past $need wanted), replace_uncopyable" \
		    "$(hv replace_uncopyable), running $([ -f $T/umltest/rm.rc.$TAG ] && echo no || echo yes)"
		[ -f $T/umltest/rm.rc.$TAG ] && { log "RM_NOT_RUNNING at the crash"; finish; }
		# A crash, with the replace running: finish syncs (the replace's
		# cursor and the log reach the disks) and powers off without an
		# unmount.
		kmsg "replace|could neither copy|raid56:" 6
		log "RM_CRASH with the replace running"
		finish
		;;
	resume-lost:1)
		echo ${RM_SPEED:-4194304} > $SPEED || log "THROTTLE_FAIL"
		rm -f $T/umltest/rm.rc.$TAG
		( btrfs replace start -B -K -f 4 $TGT $MNT > $T/umltest/rm.out.$TAG 2>&1
		  echo $? > $T/umltest/rm.rc.$TAG ) &
		n=0
		until btrfs replace status -1 $MNT 2>/dev/null | grep -q "% done"; do
			[ -f $T/umltest/rm.rc.$TAG ] && break
			n=$((n + 1)); [ $n -gt 600 ] && { log "RM_TIMEOUT"; break; }
			sleep 0.1
		done
		log "RM replace at $(progress)/1000," \
		    "running $([ -f $T/umltest/rm.rc.$TAG ] && echo no || echo yes)"
		[ -f $T/umltest/rm.rc.$TAG ] && { log "RM_NOT_RUNNING at the crash"; finish; }
		log "RM_CRASH with the replace running"
		finish
		;;
	resume:2|resume-lost:2)
		# The replace resumed at mount.
		kmsg "continuing dev_replace|copying again from the start" 2
		log "RM resumed at $(progress)/1000"
		[ $ARM = resume ] &&
			log "RM the late rows lie from" \
			    "$(sort -n -k5 $T/umltest/rm.late.$TAG | head -1 | awk '{print $5}') on device 3"
		n=0
		while btrfs replace status -1 $MNT 2>/dev/null | grep -q "% done"; do
			n=$((n + 1)); [ $n -gt 1200 ] && { log "RM_TIMEOUT"; break; }
			sleep 1
		done
		st=$(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ')
		rrc=1; case "$st" in *[Ff]inished*) rrc=0;; esac
		log "replace rc=$rrc (resumed)"
		;;
	esac
	log "replace status:" \
	    "$(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ' | cut -c1-160)"
	log "RM after the replace: stale_marks=$(hv stale_marks)" \
	    "recorded_blocks=$(hv recorded_blocks)" \
	    "sticky_blocks=$(wv sticky_blocks) sticky_evicted=$(wv sticky_evicted)"
	member=0; btrfs filesystem show $MNT 2>/dev/null | grep -q "$TGT" && member=1
	missing=0; btrfs filesystem show $MNT 2>/dev/null | grep -qi "missing" && missing=1
	ok=0; eio=0; zero=0; other=0
	while read -r i tb sd sp; do
		echo 3 > /proc/sys/vm/drop_caches
		if dd if=$MNT/nocow of=$T/umltest/rm.blk.$TAG bs=4096 skip=$tb count=1 status=none \
		      2>/dev/null; then
			if [ "$(tr -cd 'A' < $T/umltest/rm.blk.$TAG | wc -c)" = 4096 ] ||
			   [ "$(tr -cd 'B' < $T/umltest/rm.blk.$TAG | wc -c)" = 4096 ]; then
				ok=$((ok + 1))
			elif [ "$(tr -d '\000' < $T/umltest/rm.blk.$TAG | wc -c)" = 0 ]; then
				zero=$((zero + 1)); log "RM_ZERO row $i block $tb"
			else
				other=$((other + 1)); log "RM_OTHER row $i block $tb"
			fi
		else
			eio=$((eio + 1))
		fi
	done < $T/umltest/rm.score.$TAG
	kmsg "replace|could neither copy|write-intent log full|raid56:|block group ro" 10
	log "RM health: state=$(hv state) replace_uncopyable=$(hv replace_uncopyable)" \
	    "replace_record_dropped=$(hv replace_record_dropped)" \
	    "record_dropped=$(hv record_dropped)" \
	    "sticky_evicted=$(wv sticky_evicted)"
	log "RM rrc=$rrc member=$member missing=$missing rows=$(wc -l < $T/umltest/rm.score.$TAG)" \
	    "ok=$ok eio=$eio zero=$zero other=$other"
	echo "$rrc $member $missing $ok $eio $zero $other $(hv replace_record_dropped)" \
	     "$(hv replace_uncopyable)" > $T/umltest/rm.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
replace_rmw_resident)
	# A write into a full stripe a RAID5 device replace has already copied:
	# does the new device get the old device's sectors that the write's new
	# parity was computed from, or only the ones the write supplied?
	#
	# RAID5 data over three devices (two data columns a full stripe), RAID1
	# metadata, the fourth device the replace target.  Sixteen 64 KiB files
	# are preallocated one after the other (fallocate writes nothing); in
	# the first full stripe F whose column 0 is one of them, 'b', the file
	# holding column 1 is deleted, so that column 1 holds no extent.  With
	# the filesystem unmounted, all of F -- both columns and the parity --
	# is filled with random bytes, as a reused disk holds them: rows nothing
	# has written do not satisfy the parity.  Filler files, more than the
	# first data chunk holds, go into the next ones (any that land in the
	# first are deleted), so that the replace copies the first chunk at
	# once and then spends its time, throttled (scrub_speed_max), in the
	# later ones.
	#
	# 'btrfs replace start -K' of SRC, the device of F's column 1, copies
	# that column whole: free, it is rebuilt from the parity and column 0,
	# which is not what SRC holds.  With the cursor past the chunk, 16 KiB
	# of 'B' go into 'b' in place: a read-modify-write, whose new parity is
	# computed from SRC's column 1 in those rows.  After the replace, with
	# column 0's device left out, they are rebuilt from that parity and the
	# target.
	#
	# Fixed, the write puts SRC's sectors it read on the target too, and
	# 'b' reads back as 'B'.  CONTROL=1 sets
	# raid56_wf_replace_skips_resident=1: the target keeps the rebuild, and
	# 'b' reads back as something nobody wrote, with no error (nodatasum).
	watchdog ${WATCH:-900}
	dm_setup
	set -- $DMDEVS
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $1 $2 $3 || { log "MKFS_FAIL"; finish; }
	TGT=$4
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 >/dev/null 2>&1
	do_mount $OPTS,nodatasum,nossd $1
	# The write into 'b' has to go where fallocate put it.
	allow_nodatacow
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_replace_skips_resident 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: the replace gets only the sectors a write supplies"
	}
	C=$(python3 $T/umltest/raid56_chunks.py $1 2>&1)
	echo "$C" | while read -r l; do log "chunk: $l"; done
	read -r C1 C1LEN _ C1N S0 S1 S2 <<< "$(echo "$C" | awk '$3 ~ /DATA/ && $3 ~ /RAID5/ {print; exit}')"
	[ "${C1N:-0}" = 3 ] || { log "LAYOUT_FAIL: no three-device RAID5 data chunk"; finish; }
	ST=($S0 $S1 $S2)
	lg() { filefrag -v -b4096 $1 2>/dev/null | awk '/^ *0:/ {gsub(/\.\./,"",$4); print $4 * 4096; exit}'; }
	L=()
	for i in $(seq 0 15); do fallocate -l 64K $MNT/p$i; sync; L[$i]=$(lg $MNT/p$i); done
	log "preallocated at ${L[*]}"
	FI=
	for i in $(seq 0 14); do
		if [ $(( (L[i] - C1) % 131072 )) = 0 ] && [ "${L[i + 1]}" = $(( L[i] + 65536 )) ] &&
		   [ $(( L[i] + 131072 )) -le $(( C1 + C1LEN )) ]; then
			FI=$i; break
		fi
	done
	[ -n "$FI" ] || { log "LAYOUT_FAIL: no full stripe of two preallocated files"; finish; }
	F=${L[FI]}; N=$(( (F - C1) / 131072 ))
	s() { echo ${ST[$(( (N + $1) % 3 ))]} | cut -d: -f$2; }	# column (2: P) field
	D0=$(s 0 2); D0PH=$(( $(s 0 3) + N * 65536 ))
	SRC=$(s 1 2); SRCID=$(s 1 1); SRCPH=$(( $(s 1 3) + N * 65536 )); SRCEND=$(( $(s 1 3) + C1LEN / 2 ))
	PD=$(s 2 2); PDPH=$(( $(s 2 3) + N * 65536 ))
	mv $MNT/p$FI $MNT/b
	rm -f $MNT/p$((FI + 1))
	log "full stripe $F (number $N of chunk $C1, $C1LEN bytes): 'b' is column 0 on $D0 at $D0PH, column 1 on $SRC (devid $SRCID) at $SRCPH is free, P on $PD at $PDPH"
	mkdir $MNT/f
	head -c 1M /dev/urandom > /tmp/fill
	nf=$(( C1LEN / 1048576 + ${RRR_SPILL_MB:-48} ))
	for i in $(seq 0 $((nf - 1))); do cp /tmp/fill $MNT/f/$i || { log "FILL_FAIL $i"; break; }; done
	sync
	python3 $T/umltest/raid56_chunks.py $D0 2>&1 | while read -r l; do log "chunk now: $l"; done
	# The allocator may well have put all of it in later chunks already.
	del=0; kept=
	for f in $MNT/f/*; do
		in=$(filefrag -v -b4096 $f 2>/dev/null | awk -v s=$C1 -v e=$((C1 + C1LEN)) '
			/^ *[0-9]+:/ { gsub(/\.\./, "", $4); gsub(/:/, "", $5)
				       if ($4 * 4096 < s || ($5 + 1) * 4096 > e) out = 1 }
			END { print out ? 0 : 1 }')
		if [ "$in" = 1 ]; then rm -f $f; del=$((del + 1)); else kept="$kept ${f##*/}"; fi
	done
	sync; btrfs filesystem sync $MNT
	KEEP=$(echo $kept | awk '{print $NF}')
	log "filler: $nf MiB, $del in the first chunk deleted, $(echo $kept | wc -w) kept"
	[ -n "$KEEP" ] || { log "LAYOUT_FAIL: no filler outside the first chunk"; finish; }
	umount $MNT || log "UMOUNT_FAIL"
	for r in "$D0 $D0PH" "$SRC $SRCPH" "$PD $PDPH"; do
		set -- $r
		head -c 64K /dev/urandom |
			dd of=$1 bs=4096 seek=$(($2 / 4096)) count=16 iflag=fullblock \
			   oflag=direct conv=notrunc status=none || log "JUNK_FAIL $1"
	done
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $D0 $SRC $PD >/dev/null 2>&1
	do_mount $OPTS,nodatasum,nossd $D0
	FSD=$(ls -d /sys/fs/btrfs/*-*-* 2>/dev/null | head -1)
	echo ${RRR_SPEED:-1048576} > $FSD/devinfo/$SRCID/scrub_speed_max || log "THROTTLE_FAIL"
	# The replace's cursor is past the first chunk once it reaches the end of
	# its stripe on SRC; 'btrfs replace status' counts in tenths of a percent
	# of the device.
	Q=$(( $(blockdev --getsize64 $SRC) / 1000 ))
	WANT=$(( SRCEND / Q ))
	dd if=/dev/zero bs=4096 count=4 status=none | tr '\000' 'B' > /tmp/B
	btrfs replace start -B -K -f $SRC $TGT $MNT > /tmp/rep.out 2>&1 &
	RP=$!
	pm=0; st=
	for i in $(seq 1 3000); do
		st=$(btrfs replace status -1 $MNT 2>&1)
		case "$st" in *'% done'*) ;; *finished*) break;; *) sleep 0.1; continue;; esac
		pm=$(echo "$st" | sed -n 's/^\([0-9]*\)\.\([0-9]\)% done.*/\1\2/p' | sed 's/^0*\([0-9]\)/\1/')
		[ "${pm:-0}" -ge $WANT ] && break
		sleep 0.1
	done
	log "replace at ${pm:-?} per mille (the first chunk ends at $WANT): $st"
	LB=$(lg $MNT/b)
	dd if=/tmp/B of=$MNT/b bs=4096 count=4 conv=notrunc,fsync status=none || log "B_WRITE_FAIL"
	st=$(btrfs replace status -1 $MNT 2>&1)
	during=0; case "$st" in *'% done'*) during=1;; esac
	inplace=0; [ "$(lg $MNT/b)" = "$F" ] && [ "$LB" = "$F" ] && inplace=1
	log "B written: during the replace $during, in place $inplace ($st)"
	wait $RP
	rrc=$?
	log "replace rc=$rrc: $(tr '\n' ' ' < /tmp/rep.out | cut -c1-200)"
	log "replace status: $(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ' | cut -c1-160)"
	kmsg "replace|raid56:" 6
	umount $MNT || log "UMOUNT_FAIL"
	# Column 0's device left out: 'b' is rebuilt from P and the target.
	btrfs device scan --forget >/dev/null 2>&1
	btrfs device scan $PD $TGT >/dev/null 2>&1
	do_mount ro,degraded $PD
	echo 3 > /proc/sys/vm/drop_caches
	rb=$(dd if=$MNT/b bs=4096 count=4 iflag=direct status=none 2>/dev/null | wc -c)
	nb=$(dd if=$MNT/b bs=4096 count=4 iflag=direct status=none 2>/dev/null | tr -cd 'B' | wc -c)
	fok=0; cmp -s $MNT/f/$KEEP /tmp/fill && fok=1
	log "without $D0: b read $rb bytes, $nb of them 'B'; filler f/$KEEP reads back right: $fok"
	kmsg "raid56:|csum" 4
	log "RRR rrc=$rrc during=$during inplace=$inplace rb=$rb nb=$nb fok=$fok"
	echo "$rrc $during $inplace $rb $nb $fok" > $T/umltest/rrr.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
scrub_unrepaired_stop)
	# Does a RAID5 scrub go on past a full stripe it cannot repair?
	#
	# RAID5 data over three devices (two data columns a full stripe), RAID1
	# metadata.  64 KiB files of 'A', one data column each, written one
	# after the other; in a full stripe F whose column 0 is one of them, the
	# file holding column 1 is deleted, so that the last data column holds
	# no extent.  With the filesystem unmounted, row 0 of F's column 0 and
	# of its parity is overwritten: the data fails its checksum and the
	# parity cannot rebuild it.  So is row 0 of the parity of the next full
	# stripe whose parity is on the same device V -- three full stripes on,
	# two columns of 'A' -- which then describes the data wrongly, and which
	# only the scrub of V looks at.  Then 'btrfs scrub start -B'.
	#
	# Fixed, the scrub reports F unrepaired and goes on: the later parity
	# on V is regenerated (zeros).  CONTROL=1 sets
	# raid56_scrub_unrepaired_stops=1: the scrub of V takes F for the end
	# of the chunk, and the later parity stays wrong, with nothing said
	# about it.
	watchdog ${WATCH:-600}
	dm_setup
	set -- $DMDEVS
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $1 $2 $3 || { log "MKFS_FAIL"; finish; }
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $1 $2 $3 >/dev/null 2>&1
	do_mount $OPTS,nossd $1
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_scrub_unrepaired_stops 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: an unrepaired full stripe ends the scrub of its chunk"
	}
	C=$(python3 $T/umltest/raid56_chunks.py $1 2>&1)
	echo "$C" | while read -r l; do log "chunk: $l"; done
	read -r C1 C1LEN _ C1N S0 S1 S2 <<< "$(echo "$C" | awk '$3 ~ /DATA/ && $3 ~ /RAID5/ {print; exit}')"
	[ "${C1N:-0}" = 3 ] || { log "LAYOUT_FAIL: no three-device RAID5 data chunk"; finish; }
	ST=($S0 $S1 $S2)
	lg() { filefrag -v -b4096 $1 2>/dev/null | awk '/^ *0:/ {gsub(/\.\./,"",$4); print $4 * 4096; exit}'; }
	dd if=/dev/zero bs=64K count=1 status=none | tr '\000' 'A' > /tmp/A64
	L=()
	for i in $(seq 0 23); do cp /tmp/A64 $MNT/w$i; sync; L[$i]=$(lg $MNT/w$i); done
	log "written at ${L[*]}"
	FI=
	for i in $(seq 0 16); do
		[ $(( (L[i] - C1) % 131072 )) = 0 ] || continue
		[ $(( L[i] + 4 * 131072 )) -le $(( C1 + C1LEN )) ] || continue
		ok=1
		for k in $(seq 1 7); do [ "${L[i + k]}" = $(( L[i] + k * 65536 )) ] || ok=0; done
		[ $ok = 1 ] && { FI=$i; break; }
	done
	[ -n "$FI" ] || { log "LAYOUT_FAIL: no four full stripes of files one after the other"; finish; }
	F=${L[FI]}; N=$(( (F - C1) / 131072 ))
	s() { echo ${ST[$(( (N + $1) % 3 ))]} | cut -d: -f$2; }	# column (2: P) field
	D0=$(s 0 2); D0PH=$(( $(s 0 3) + N * 65536 ))
	V=$(s 2 2); VPH=$(( $(s 2 3) + N * 65536 ))
	# Full stripe N + 3 has its parity on the same chunk stripe.
	VPH3=$(( $(s 2 3) + (N + 3) * 65536 ))
	rm -f $MNT/w$((FI + 1))
	sync; btrfs filesystem sync $MNT
	log "full stripe $F (number $N): column 0 on $D0 at $D0PH, column 1 now free, P on $V at $VPH; full stripe $((F + 3 * 131072))'s P at $VPH3"
	umount $MNT || log "UMOUNT_FAIL"
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'Z' > /tmp/Z
	for r in "$D0 $D0PH" "$V $VPH" "$V $VPH3"; do
		set -- $r
		dd if=/tmp/Z of=$1 bs=4096 seek=$(($2 / 4096)) count=1 oflag=direct \
		   conv=notrunc status=none || log "CORRUPT_FAIL $1 $2"
	done
	btrfs device scan --forget >/dev/null 2>&1; btrfs device scan $DMDEVS >/dev/null 2>&1
	do_mount $OPTS,nossd $D0
	btrfs scrub start -B $MNT > /tmp/scrub.out 2>&1
	src=$?
	log "scrub rc=$src: $(tr '\n' ' ' < /tmp/scrub.out | grep -o 'Error summary:.*' | cut -c1-160)"
	sv() { sed -n "s/^ *$1: *\([0-9]*\).*/\1/p" /tmp/scrub.out | head -1; }
	corr=$(sv Corrected); unc=$(sv Uncorrectable)
	unrep=$(dmesg | grep -c "unrepaired sectors detected, full stripe $F ")
	kmsg "scrub|raid56:" 6
	umount $MNT || log "UMOUNT_FAIL"
	pz=$(dd if=$V bs=4096 skip=$((VPH3 / 4096)) count=1 iflag=direct status=none | tr -cd '\000' | wc -c)
	pZ=$(dd if=$V bs=4096 skip=$((VPH3 / 4096)) count=1 iflag=direct status=none | tr -cd 'Z' | wc -c)
	log "SUS rc=$src corrected=${corr:-?} uncorrectable=${unc:-?} unrepaired_msgs=$unrep later_parity_zero=$pz later_parity_Z=$pZ"
	echo "$src ${corr:-?} ${unc:-?} $unrep $pz $pZ" > $T/umltest/sus.$TAG
	dmsetup remove_all 2>/dev/null
	finish
	;;
readd_flush_prep)
	# A device that acknowledges writes into a cache it then loses, and says
	# so the only way a device can: by failing the next cache flush.
	#
	# RAID5 data, RAID1 metadata, four devices.  A nodatacow file of 'A' is
	# committed; then device FAIL takes every write and throws it away
	# (dm-flakey drop_writes -- the volatile cache that is about to be lost)
	# while one 4 KiB block per full stripe is overwritten in place with
	# 'B', with O_DIRECT and no fsync, so no barrier runs meanwhile: a
	# barrier to a device that drops writes "succeeds" and would let the
	# log drop those stripes before anything failed.  The writes are
	# acknowledged and the log records their stripes; on FAIL the data or
	# parity column of about half of them never lands.  Then FAIL fails
	# writes and flushes (error_writes) and a commit runs: its barrier fails
	# on FAIL, so the log keeps the stripes it was about to drop.  Power off
	# without unmounting; readd_flush_verify reads them back.
	#
	# Fixed, the kept stripes name FAIL's column or parity (stale /
	# stale_par) and the next mount rebuilds FAIL's column from the parity.
	# CONTROL=1 sets raid56_wf_no_readd_name=1: they are kept as records that
	# do not say which device, and FAIL's column is read back as it is.
	#
	# DISABLE=1: the log is being disabled, and the commit whose barrier
	# fails is the one that writes its last block (wib_write_final_locked()).
	# The disable takes two commits: the first writes a superblock without
	# the feature flag, the next the last block.  So the first runs before
	# the overwrites, the log still recording them, and the second is the
	# failed one.  CONTROL=2 sets raid56_wf_disable_forgets_writes=1: the
	# last block ignores the failed barrier and lists nothing it dropped.
	watchdog ${WATCH:-600}
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_no_readd_name 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: a failed flush keeps records that do not name the device"
	}
	[ "${CONTROL:-0}" = 2 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_disable_forgets_writes \
			2>/dev/null || log "CONTROL_KNOB_FAIL"
		log "control: the disable's last block forgets what its failed barrier dropped"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	lsattr $MNT/nocow 2>/dev/null | grep -q C || log "NOT_NODATACOW"
	dd if=/dev/zero bs=1M count=$(( RF_BLOCKS * NOCOW_FS_BLOCKS * 4096 / 1048576 + 2 )) \
		status=none | tr '\000' 'A' > $MNT/nocow
	sync
	log "nocow layout: $(filefrag -v $MNT/nocow | sed -n 4p | tr -s ' ')"
	W=$(ls /sys/fs/btrfs/*-*-*/raid56_write_intent 2>/dev/null | head -1)
	if [ "${DISABLE:-0}" = 1 ]; then
		echo 0 > /sys/fs/btrfs/*-*-*/features/raid56_write_intent || log "DISABLE_FAIL"
		touch $MNT/marker0
		sync
		# Still enabled: only the superblock without the flag is written.
		[ "$(awk '$1 == "enabled" {print $2}' $W)" = 1 ] || log "RF_DISABLED_EARLY"
		log "disable requested, log enabled until the next commit:" \
		    "$(awk '$1 == "enabled" {print $2}' $W)"
	fi
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > /tmp/bblock.rf
	CS=$(ls /sys/fs/btrfs/*-*-*/commit_stats 2>/dev/null | head -1)
	commits0=$(awk '$1 == "commits" {print $2}' $CS 2>/dev/null)
	dm_drop_writes $FAIL; log "device $FAIL drops every write"
	acked=0; : > $T/umltest/nocow.acked.$TAG
	for i in $(seq 0 $((RF_BLOCKS-1))); do
		dd if=/tmp/bblock.rf of=$MNT/nocow bs=4096 seek=$((i * NOCOW_FS_BLOCKS)) count=1 \
		   oflag=direct conv=notrunc status=none 2>/dev/null &&
			{ acked=$((acked+1)); echo $i >> $T/umltest/nocow.acked.$TAG; }
	done
	commits1=$(awk '$1 == "commits" {print $2}' $CS 2>/dev/null)
	log "in-place overwrites: $acked of $RF_BLOCKS acknowledged (commits $commits0 -> $commits1)"
	# A commit while the device dropped writes had a barrier it "confirmed":
	# the stripes may be gone from the log before anything failed.
	[ "$commits0" = "$commits1" ] || log "RF_COMMIT_WHILE_DROPPING"
	dm_error_writes $FAIL; log "device $FAIL fails writes and flushes"
	touch $MNT/marker
	sync
	fl=$(btrfs device stats $MNT 2>/dev/null | awk '/flush_io_errs/ {s += $2} END {print s + 0}')
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	stats "after the failed flush"
	kmsg "did not confirm|write-intent log|raid56:" 6
	if [ "${DISABLE:-0}" = 1 ]; then
		[ "$(awk '$1 == "enabled" {print $2}' $W)" = 0 ] || log "RF_NOT_DISABLED"
		dmesg | grep -q "raid56 write-intent log disabled" || log "RF_NOT_DISABLED"
	fi
	log "RF_PREP acked=$acked flush_errs=$fl stale_marks=$(hv stale_marks) recorded=$(hv recorded_blocks) state=$(hv state) log_flush_unnamed=$(hv log_flush_unnamed)"
	echo "$acked $fl $(hv stale_marks) $([ "$commits0" = "$commits1" ] && echo 0 || echo 1)" \
		> $T/umltest/rf-prep.$TAG
	# No umount: the crash.  finish syncs once more, into the same failing
	# barrier, and powers off.
	finish
	;;
readd_flush_verify)
	# The disks as they were left, no device-mapper: FAIL holds the old
	# content wherever its writes were dropped.  Mount (the log's recovery
	# runs), then read back every overwritten block and, as a check that
	# nothing else was damaged on the way, the two blocks 64 and 128 KiB
	# after it, which nobody overwrote -- a false stale name on one of them
	# is rebuilt from the parity, and must still read back as 'A' or fail
	# (side_eio: refused, counted apart from side_bad: read back wrong).
	#   new     reads back as the acknowledged 'B'
	#   old     reads back as the old 'A', with no error: silently wrong
	#   eio     the read failed: refused, not wrong
	#   other   anything else: a value nobody wrote
	do_mount $OPTS $MNTDEV
	kmsg "write-intent|raid56:" 8
	stats "after recovery"
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	log "raid56_health: $(tr '\n' ' ' < $H 2>/dev/null | cut -c1-300)"
	acked=" $(tr '\n' ' ' < $T/umltest/nocow.acked.$TAG 2>/dev/null) "
	new=0; old=0; eio=0; other=0; side_bad=0; side_eio=0
	for i in $(seq 0 $((RF_BLOCKS-1))); do
		off=$((i * NOCOW_FS_BLOCKS))
		n=$(dd if=$MNT/nocow bs=4096 skip=$off count=1 status=none 2>/dev/null | wc -c)
		nb=$(dd if=$MNT/nocow bs=4096 skip=$off count=1 status=none 2>/dev/null | tr -cd 'B' | wc -c)
		na=$(dd if=$MNT/nocow bs=4096 skip=$off count=1 status=none 2>/dev/null | tr -cd 'A' | wc -c)
		if [ "$n" != 4096 ]; then
			eio=$((eio+1)); log "RF_BLOCK $i EIO"
		elif [ "$nb" = 4096 ]; then
			new=$((new+1))
		elif [ "$na" = 4096 ]; then
			# Only an acknowledged overwrite promised 'B'.
			case "$acked" in *" $i "*) old=$((old+1)); log "RF_BLOCK $i OLD";; esac
		else
			other=$((other+1)); log "RF_BLOCK $i OTHER"
		fi
		for s in 16 32; do
			if dd if=$MNT/nocow of=$T/umltest/blk.$TAG bs=4096 skip=$((off + s)) \
			      count=1 status=none 2>/dev/null; then
				na=$(tr -cd 'A' < $T/umltest/blk.$TAG | wc -c)
				[ "$na" = 4096 ] ||
					{ side_bad=$((side_bad+1)); log "RF_SIDE $i+$s BAD"; }
			else
				side_eio=$((side_eio+1)); log "RF_SIDE $i+$s EIO"
			fi
		done
	done
	rm -f $T/umltest/blk.$TAG
	log "RF new=$new old=$old eio=$eio other=$other side_bad=$side_bad" \
	    "side_eio=$side_eio of $RF_BLOCKS"
	echo "$new $old $eio $other $side_bad $side_eio" > $T/umltest/rf.$TAG
	kmsg "raid56|csum|error" 6
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
torn_readd_prep)
	# A write a failed flush may have torn, whose record the log could not
	# name the device in: after a crash, is it still treated as a write
	# that may have been torn?
	#
	# RAID5 data, RAID1C3 metadata (so that with one device out and another
	# having failed writes a copy of every tree block is left), four
	# devices.  A nodatacow file of 'A' is committed; then device FAIL
	# takes every write and throws it away (dm-flakey drop_writes: the
	# volatile cache about to be lost) while one 4 KiB block per region is
	# overwritten in place with 'B' (O_DIRECT, no fsync, no commit): about
	# half of those writes put their column or the parity on FAIL, which
	# never gets it.  Then FAIL fails writes and flushes (error_writes) and
	# a commit runs: its barrier fails on FAIL, and the log keeps every
	# stripe it was about to drop -- but ninety-odd regions with a name are
	# more than a block that names anything can describe, so it keeps them
	# without the names (log_flush_unnamed) and marks the ones FAIL got a
	# write in possibly torn.  FAIL is healed and a commit whose barrier
	# every device confirms drops what was in flight: from here on only
	# the mark says those writes may be torn.  Power off without unmounting.
	#
	# CONTROL=1 sets raid56_wf_torn_no_persist=1: the mark stays in memory,
	# and the records go to the disk as plain failed writes.  CONTROL=2, 3
	# and 4 (the upgrade pairs) set raid56_wf_no_readd_name=1 and
	# raid56_wf_log_unmarked=1: the log a kernel from before the mark leaves,
	# its records plain, its blocks without BTRFS_WIB_TORN_MARKING.
	watchdog ${WATCH:-1500}
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	echo 600000 > /sys/module/btrfs/parameters/raid56_repair_delay_ms
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_torn_no_persist 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: the possibly-torn mark is not written to the log"
	}
	case "${CONTROL:-0}" in 2|3|4)
		{ echo 1 > /sys/module/btrfs/parameters/raid56_wf_no_readd_name &&
		  echo 1 > /sys/module/btrfs/parameters/raid56_wf_log_unmarked; } 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "upgrade: the log as a kernel from before the mark leaves it"
	esac
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	lsattr $MNT/nocow 2>/dev/null | grep -q C || log "NOT_NODATACOW"
	dd if=/dev/zero bs=1M count=$(( TR_REGIONS * TR_STRIDE * 4096 / 1048576 + 2 )) \
		status=none | tr '\000' 'A' > $MNT/nocow
	sync
	python3 $T/umltest/raid56_rows.py $MNT/nocow /dev/mapper/d0 $TR_STRIDE $TR_REGIONS \
		> $T/umltest/tr.rows.$TAG 2>&1
	n=$(awk 'NF > 4 && $3 >= 0' $T/umltest/tr.rows.$TAG | wc -l)
	[ "$n" = "$TR_REGIONS" ] || {
		log "LAYOUT_FAIL $n of $TR_REGIONS rows:" \
		    "$(head -2 $T/umltest/tr.rows.$TAG | tr '\n' ' ')"
		finish
	}
	# verdict_keep: room for later writes into new regions, preallocated
	# while every device is there, so that its full stripes have a column
	# on the device the verify mounts leave out.
	if [ -n "${VK_SPARE:-}" ]; then
		touch $MNT/spare; chattr +C $MNT/spare
		fallocate -l $(( (VK_SPARE + 1) * 4 ))M $MNT/spare || log "FALLOCATE_FAIL spare"
		sync
	fi
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > /tmp/bblock.tr
	CS=$(ls /sys/fs/btrfs/*-*-*/commit_stats 2>/dev/null | head -1)
	commits0=$(awk '$1 == "commits" {print $2}' $CS 2>/dev/null)
	dm_drop_writes $FAIL; log "device $FAIL drops every write"
	acked=0; : > $T/umltest/tr.acked.$TAG
	for i in $(seq 0 $((TR_REGIONS-1))); do
		dd if=/tmp/bblock.tr of=$MNT/nocow bs=4096 seek=$((i * TR_STRIDE)) count=1 \
		   oflag=direct conv=notrunc status=none 2>/dev/null &&
			{ acked=$((acked+1)); echo $i >> $T/umltest/tr.acked.$TAG; }
	done
	commits1=$(awk '$1 == "commits" {print $2}' $CS 2>/dev/null)
	log "in-place overwrites: $acked of $TR_REGIONS acknowledged" \
	    "(commits $commits0 -> $commits1)"
	[ "$commits0" = "$commits1" ] || log "TR_COMMIT_WHILE_DROPPING"
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	W=$(ls /sys/fs/btrfs/*-*-*/raid56_write_intent 2>/dev/null | head -1)
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	wv() { awk -v k=$1 '$1 == k {print $2}' $W 2>/dev/null; }
	dm_error_writes $FAIL; log "device $FAIL fails writes and flushes"
	touch $MNT/marker
	sync
	fl=$(btrfs device stats $MNT 2>/dev/null |
	     awk '/flush_io_errs/ {s += $2} END {print s + 0}')
	unnamed=$(hv log_flush_unnamed); torn1=$(wv torn_blocks)
	kmsg "did not confirm|write-intent log|raid56:" 6
	dm_heal $FAIL; log "healed device $FAIL"
	touch $MNT/marker2
	sync
	torn2=$(wv torn_blocks); sticky2=$(wv sticky_blocks)
	stats "after the drop"
	log "TR_PREP acked=$acked flush_errs=$fl log_flush_unnamed=$unnamed" \
	    "torn_blocks=$torn1 then $torn2 sticky_blocks=$sticky2"
	echo "$acked $fl ${unnamed:-0} ${torn1:-0} ${torn2:-0}" \
	     "$([ "$commits0" = "$commits1" ] && echo 0 || echo 1) ${sticky2:-0}" \
	     > $T/umltest/tr-prep.$TAG
	# No umount: the crash.
	finish
	;;
torn_readd_verify)
	# The disks as the crash left them, no device-mapper, device OMITTED
	# left out and mounted degraded: PHASE=ro first (no recovery: the log
	# read at mount decides), then PHASE=rw (the recovery meets the
	# records).  Every row an overwrite rewrote the parity of is read
	# back where it lies on OMITTED -- rebuilt from that parity:
	#   ok     what was acknowledged ('B' for the overwritten block, else 'A')
	#   eio    the read failed: refused, not wrong
	#   wrong  anything else: a rebuild from a parity the dropped write left
	#          not describing the data, handed back as the file's content
	# Where the row lies on FAIL, the block is read as FAIL has it: a block
	# whose write FAIL dropped reads as the old 'A' there, which the log
	# could not name (log_flush_unnamed) -- counted apart, not scored.
	# Rows on the other devices must read what was acknowledged (direct).
	# CONTROL=3: raid56_wf_trust_unmarked_log=1, the log's plain records are
	# read as failed writes although its blocks do not say their writer
	# marked the others.
	# CONTROL=4: raid56_wf_suspect_as_stale=1, the recovery's verdict on a
	# stripe it cannot decide counts as a stale parity: it makes the log
	# block wide, and a full log with a device missing spends it in table
	# order.
	[ "${CONTROL:-0}" = 3 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_trust_unmarked_log 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: a log without the marker is trusted"
	}
	[ "${CONTROL:-0}" = 4 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_suspect_as_stale 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: the recovery's verdicts count as stale parities"
	}
	do_mount $OPTS,degraded $MNTDEV
	kmsg "write-intent|scrub: full stripe|possibly torn" 8
	kmsg "does not mark the writes" 1
	stats
	# What the log listed at mount; a read-write mount's recovery has
	# taken it over by now (0).
	regs=$(sed -n 's/^pending_recovery_regions //p' /sys/fs/btrfs/*/raid56_write_intent |
	       head -1)
	tr_score
	sus=$(sed -n 's/^recovery_suspect //p' /sys/fs/btrfs/*/raid56_write_intent | head -1)
	ev=$(sed -n 's/^sticky_evicted //p' /sys/fs/btrfs/*/raid56_write_intent | head -1)
	amb=$(sed -n 's/^read_unverifiable //p' /sys/fs/btrfs/*/raid56_health | head -1)
	kmsg "refusing a read|possibly torn|recorded as possibly torn|log full, dropping" 4
	log "TR phase $PHASE: checked=$checked ok=$ok eio=$eio wrong=$wrong torn_rows=$torn_rows" \
	    "fail_old=$fail_old direct_bad=$direct_bad recovery_suspect=${sus:-?}" \
	    "read_unverifiable=${amb:-?} sticky_evicted=${ev:-?} regions=${regs:-?}"
	echo "$checked $ok $eio $wrong $torn_rows $fail_old $direct_bad ${sus:-?} ${ev:-?}" \
	     "${regs:-?}" > $T/umltest/tr.$TAG.$PHASE
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
readd_admit)
	# A failed flush's readd owed while writes into new regions keep coming:
	# does the log get out of it once the device works again?
	#
	# RAID5 data, RAID1 metadata on the first four devices; a nodatacow file
	# preallocated there, then the fifth device added, which holds no member
	# of the file's stripes -- so a flush it fails names nothing, and every
	# record the readd takes back is a plain one.  One block per region is
	# written into RA_LAST regions, one after the other, without a
	# transaction commit (commit=600): the log's last block lists them all,
	# close to the 165 a block holds.  Then the fifth device fails every
	# flush (its writes still land), and RA_WRITERS writers write into
	# regions of their own that the last block does not list, RA_FAIL_SECS
	# long; then it is healed, and they go on for RA_HEAL_SECS.  Each write
	# is scored by when it started: while failing, just after the heal, or
	# RA_SETTLE_SECS after it ("late").
	#   fixed    no write into a region the last block does not list is
	#            recorded while the readd is owed; the writers wait, the
	#            readd is done once the writes holding its room are, and
	#            every late write succeeds
	#   control  raid56_wf_readd_admits_new=1: they are recorded, every
	#            write adds a region no block can take besides the last
	#            one's, and writes keep failing (log_write_failed) as long as
	#            the writers write, device healed or not
	# RA_HOT=1: RA_SHARE writers to a region instead, each rewriting a
	# block of a full stripe of its own there over and over from a single
	# process (no fork per write), so that the regions stay busy: those the
	# last block does not list are what an owed readd waits for.  The device
	# is healed at its first failed flush, as a transient failure heals.
	#   fixed    a write into such a region waits as a write into a new one
	#            does; the readd is done once the writes in flight there are,
	#            and every late write succeeds with no record lost after the
	#            heal
	#   control  (CONTROL=2) raid56_wf_readd_admits_busy=1: they are
	#            recorded and keep the regions busy, the readd waits until
	#            BTRFS_WIB_READD_WAIT, late writes fail meanwhile, and then
	#            records are lost (record_dropped)
	# RA_SAY=1: the kernel log is followed to the end, and the lines that
	# say an owed readd's wait for room begins and ends are counted.
	#   fixed    one wait said per minute, with how many went unsaid; an
	#            end said only for a wait that was
	#   control  (CONTROL=3) raid56_wf_readd_says_each=1: every wait and
	#            every end, one pair per failed flush
	watchdog ${WATCH:-900}
	dm_setup
	set -- $DMDEVS
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $1 $2 $3 $4 || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_readd_admits_new 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: writes into new regions are recorded while the readd is owed"
	}
	[ "${CONTROL:-0}" = 2 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_readd_admits_busy 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: writes into busy regions are recorded while the readd is owed"
	}
	[ "${CONTROL:-0}" = 3 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_readd_says_each 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: every wait of the readd and its end are said"
	}
	RA_LAST=${RA_LAST:-150}; RA_WRITERS=${RA_WRITERS:-8}; RA_PER=${RA_PER:-4}
	RA_SHARE=${RA_SHARE:-2}
	RA_STRIDE=1040		# 4 KiB blocks: 4 MiB and 64 KiB, one region each
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	fallocate -l $(( (RA_LAST + RA_WRITERS * RA_PER + 1) * RA_STRIDE * 4096 )) $MNT/nocow ||
		{ log "FALLOCATE_FAIL"; finish; }
	sync
	btrfs device add -f /dev/mapper/d4 $MNT > /dev/null 2>&1 || { log "DEVADD_FAIL"; finish; }
	sync
	log "fs devices: $(btrfs filesystem show $MNT 2>/dev/null | grep devid | tr -s ' ' | tr '\n' ';')"
	W=$(ls /sys/fs/btrfs/*-*-*/raid56_write_intent 2>/dev/null | head -1)
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	CS=$(ls /sys/fs/btrfs/*-*-*/commit_stats 2>/dev/null | head -1)
	wv() { awk -v k=$1 '$1 == k {print $2}' $W 2>/dev/null; }
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	BB=$T/umltest/bblock.$TAG
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > $BB
	commits0=$(awk '$1 == "commits" {print $2}' $CS 2>/dev/null)
	flushes0=$(wv commit_flushes)
	for i in $(seq 0 $((RA_LAST - 1))); do
		dd if=$BB of=$MNT/nocow bs=4096 seek=$((i * RA_STRIDE)) count=1 \
		   oflag=direct conv=notrunc status=none 2>/dev/null || log "RA_FILL_FAIL $i"
	done
	log "RA filled: commits $commits0 -> $(awk '$1 == "commits" {print $2}' $CS 2>/dev/null)," \
	    "log flushes $flushes0 -> $(wv commit_flushes)"
	[ "$flushes0" = "$(wv commit_flushes)" ] || log "RA_FILL_FLUSHED"
	# The fifth device fails its flushes: writes to 4 MiB near its end
	# fail, which nothing uses, and a flush goes to every part of it.
	sz=$(blockdev --getsize64 /dev/mapper/d4)
	# The log's own flushes that fail, counted from a follower of the
	# kernel log: no transaction commit runs, whose barrier would count
	# in flush_io_errs, and in the 16 KiB buffer the messages about the
	# readd's records soon scroll the failures away.
	KM=$T/umltest/ra.kmsg.$TAG
	stdbuf -oL dmesg -w > $KM 2>/dev/null &
	kpid=$!
	dm_error_writes_range 4 $((sz - 8388608)) 4194304
	log "device 4 fails every flush"
	PH=$T/umltest/ra.phase.$TAG; STOP=$T/umltest/ra.stop.$TAG
	echo fail > $PH; rm -f $STOP $STOP.go $T/umltest/ra.w*.$TAG*
	# RA_HOT: one process per writer, one line per write when it stops.
	cat > /tmp/ra_hot.py <<'PYEOF'
import mmap, os, sys, time
path, off, ph, stop, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
size = int(sys.argv[6])
buf = mmap.mmap(-1, size)
buf.write(b'B' * size)
fd = os.open(path, os.O_WRONLY | os.O_DIRECT)
res = []
# Ready, then all at once: a writer started late finds its region drained.
open(out + '.ready', 'w').close()
while not os.path.exists(stop + '.go'):
    time.sleep(0.001)
while not os.path.exists(stop):
    with open(ph) as f:
        p = f.read().strip()
    try:
        os.pwrite(fd, buf, off)
        res.append(p + ' ok')
    except OSError:
        res.append(p + ' eio')
with open(out, 'w') as f:
    f.write('\n'.join(res) + '\n')
PYEOF
	pids=""
	for w in $(seq 0 $((RA_WRITERS - 1))); do
		if [ "${RA_HOT:-0}" = 1 ]; then
			# Each writer a block of a full stripe of its own in
			# its region (48 blocks: the three data columns of the
			# first four devices), or the stripe lock would take
			# turns for them.
			r=$(( RA_LAST + w / RA_SHARE ))
			python3 /tmp/ra_hot.py $MNT/nocow \
				$(( (r * RA_STRIDE + (w % RA_SHARE) * 48) * 4096 )) \
				$PH $STOP $T/umltest/ra.w$w.$TAG 4096 &
			pids="$pids $!"
			continue
		fi
		(
		j=0
		while [ ! -e $STOP ]; do
			r=$(( RA_LAST + w + RA_WRITERS * (j % RA_PER) )); j=$((j + 1))
			p=$(cat $PH)
			if dd if=$BB of=$MNT/nocow bs=4096 seek=$((r * RA_STRIDE)) count=1 \
			      oflag=direct conv=notrunc status=none 2>/dev/null; then
				echo "$p ok"
			else
				echo "$p eio"
			fi >> $T/umltest/ra.w$w.$TAG
		done
		) &
		pids="$pids $!"
	done
	if [ "${RA_HOT:-0}" = 1 ]; then
		for i in $(seq 600); do
			[ "$(ls $T/umltest/ra.w*.$TAG.ready 2>/dev/null | wc -l)" = $RA_WRITERS ] &&
				break
			sleep 0.1
		done
		touch $STOP.go
		for i in $(seq $(( ${RA_FAIL_SECS:-5} * 20 ))); do
			grep -q "write-intent log flush failed" $KM && break
			sleep 0.05
		done
	else
		sleep ${RA_FAIL_SECS:-5}
	fi
	fl=$(grep -c "write-intent log flush failed" $KM)
	[ "${RA_HOT:-0}" = 1 ] || [ "${RA_SAY:-0}" = 1 ] ||
		{ kill $kpid 2>/dev/null; wait $kpid 2>/dev/null; }
	dm_heal 4; echo heal > $PH; log "healed device 4"
	dropped_heal=$(hv record_dropped)
	sleep ${RA_SETTLE_SECS:-5}
	echo late > $PH
	sleep $(( ${RA_HEAL_SECS:-25} - ${RA_SETTLE_SECS:-5} ))
	# Not a bare wait: the watchdog is a child too.
	touch $STOP; wait $pids
	gave_up=0
	if [ "${RA_HOT:-0}" = 1 ]; then
		gave_up=$(grep -c "not waiting for them any longer" $KM)
		kill $kpid 2>/dev/null; wait $kpid 2>/dev/null
		log "RA_HOT the readd gave up waiting $gave_up time(s)"
	fi
	if [ "${RA_SAY:-0}" = 1 ]; then
		sleep 1
		kill $kpid 2>/dev/null; wait $kpid 2>/dev/null
		waits=$(grep -c "dropping nothing from it until then" $KM)
		ends=$(grep -c "took back the records a failed flush kept" $KM)
		unsaid=$(sed -n 's/.*(and \([0-9]*\) more times since this was last said).*/\1/p' $KM |
			 awk '{s += $1} END {print s + 0}')
		log "RA_SAY waits said $waits (reported unsaid $unsaid), ends said $ends," \
		    "log flushes failed $fl"
		echo "$waits $unsaid $ends $fl" > $T/umltest/ra.say.$TAG
	fi
	cat $T/umltest/ra.w*.$TAG > $T/umltest/ra.all.$TAG
	n() { grep -c "^$1 $2\$" $T/umltest/ra.all.$TAG; }
	commits1=$(awk '$1 == "commits" {print $2}' $CS 2>/dev/null)
	stats "end"
	kmsg "did not confirm|write-intent log|raid56:" 8
	log "RA fail ok=$(n fail ok) eio=$(n fail eio) heal ok=$(n heal ok) eio=$(n heal eio)" \
	    "late ok=$(n late ok) eio=$(n late eio) log_flush_errs=$fl commits $commits0 -> $commits1" \
	    "log_write_failed=$(hv log_write_failed) record_dropped=$(hv record_dropped)" \
	    "(at the heal $dropped_heal) log_full=$(hv log_full) state=$(hv state)"
	echo "$(n fail ok) $(n fail eio) $(n heal ok) $(n heal eio) $(n late ok) $(n late eio)" \
	     "$fl $([ "$commits0" = "$commits1" ] && echo 0 || echo 1) $(hv log_write_failed)" \
	     "$(( $(hv record_dropped) - dropped_heal )) $gave_up" > $T/umltest/ra.$TAG
	rm -f $BB $PH $STOP $STOP.go $KM $T/umltest/ra.w*.$TAG* /tmp/ra_hot.py
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
flush_wedge)
	# Records a failed flush leaves, with every device there: do they wedge
	# the log once it is full of them?
	#
	# RAID5 data, RAID1 metadata, four devices.  A nodatacow file
	# preallocated, one region (4 MiB) per row as torn_readd_prep lays them
	# out, of those rows the ones whose full stripe lies in one region;
	# device FAIL takes every write and throws it away while one block per
	# region is overwritten in place, chosen so that FAIL holds its data
	# column or the parity of its row: every region gets a name.  Then FAIL
	# fails writes and flushes, a commit's barrier fails on it, and the
	# readd keeps every region with FAIL named -- or, PLAN=torn, more than
	# a block that names anything holds: without the names, each marked
	# possibly torn.  Either way the log is full of them.  FAIL is healed,
	# a commit drops what it can, and FW_FRESH writes go into new regions,
	# one at a time.  Then every overwritten block is read back.
	#   PLAN=named (FW_REGIONS 82: a wide block holds 82)
	#     fixed    the readd asks for a repair of every stripe it named, and
	#              says so (device_write_failed); the repairs retire the
	#              records, and the writes into new regions succeed
	#     control  raid56_wf_readd_no_repair=1: nothing retires them, and
	#              the writes fail at once (log_full)
	#   PLAN=torn (FW_REGIONS 165: a narrow block holds 165)
	#     fixed    a full log spends the possibly torn records, last and with
	#              the alert (record_dropped), and the writes succeed
	#     control  raid56_wf_torn_unevictable=1: it keeps them, and the
	#              writes fail at once (log_full)
	#   PLAN=busy (FW_REGIONS 164: one slot is left) and, instead of the
	#            writes one at a time, FW_BUSY at once into rows of new
	#            regions whose parity FAIL holds, started while FAIL is
	#            suspended for FW_HOLD_SECS: they queue behind it and are
	#            let go together, so that the first to be recorded takes the
	#            free slot and the others find the log full with a write in
	#            flight, which frees the slot when it finishes.  Mount with
	#            enough thread_pool for them all to get that far at once.
	#     fixed    they wait for it and take its slot in turn: no record is
	#              spent, every write succeeds
	#     control  raid56_wf_torn_spent_eagerly=1: each spends a possibly
	#              torn record at once (sticky_evicted, record_dropped)
	watchdog ${WATCH:-900}
	dm_setup
	mkfs.btrfs -K -q -f -d raid5 -m raid1 $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	case "$PLAN" in
	named) knob=raid56_wf_readd_no_repair; FW_REGIONS=${FW_REGIONS:-82};;
	torn) knob=raid56_wf_torn_unevictable; FW_REGIONS=${FW_REGIONS:-165};;
	busy) knob=raid56_wf_torn_spent_eagerly; FW_REGIONS=${FW_REGIONS:-164};;
	*) log "PLAN_UNKNOWN $PLAN"; finish;;
	esac
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/$knob 2>/dev/null || log "CONTROL_KNOB_FAIL"
		log "control: $knob=1"
	}
	FW_FRESH=${FW_FRESH:-20}; FW_BUSY=${FW_BUSY:-8}
	nrows=$((FW_REGIONS + FW_FRESH + 40))
	[ "$PLAN" = busy ] && nrows=$((nrows + 4 * FW_BUSY + 8))
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	fallocate -l $(( (nrows + 1) * TR_STRIDE * 4096 )) $MNT/nocow ||
		{ log "FALLOCATE_FAIL"; finish; }
	sync
	R=$T/umltest/fw.rows.$TAG
	python3 $T/umltest/raid56_rows.py $MNT/nocow /dev/mapper/d0 $TR_STRIDE $nrows region \
		> $R 2>&1
	# Per region, in rows whose full stripe lies in it: a block whose data
	# column is on FAIL, or any if FAIL holds the parity of its row.  The
	# first FW_REGIONS are overwritten, the next FW_FRESH are the new ones.
	k=0
	while read -r i fblk col pdev reg mates; do
		[ "$col" -ge 0 ] 2>/dev/null && [ "${reg##*:}" = 0 ] || continue
		b=-1
		if [ "$pdev" = "$FAIL" ]; then
			b=$fblk
		else
			for m in $mates; do [ "${m##*:}" = "$FAIL" ] && b=${m%%:*}; done
		fi
		[ "$b" -ge 0 ] || continue
		echo "$k $b $fblk"; k=$((k + 1))
	done < $R > $T/umltest/fw.targets.$TAG
	n=$(wc -l < $T/umltest/fw.targets.$TAG)
	[ "$n" -ge $((FW_REGIONS + FW_FRESH)) ] || {
		log "LAYOUT_FAIL $n of $((FW_REGIONS + FW_FRESH)) rows: $(head -2 $R | tr '\n' ' ')"
		finish
	}
	W=$(ls /sys/fs/btrfs/*-*-*/raid56_write_intent 2>/dev/null | head -1)
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	CS=$(ls /sys/fs/btrfs/*-*-*/commit_stats 2>/dev/null | head -1)
	wv() { awk -v k=$1 '$1 == k {print $2}' $W 2>/dev/null; }
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	BB=$T/umltest/bblock.$TAG
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > $BB
	commits0=$(awk '$1 == "commits" {print $2}' $CS 2>/dev/null)
	dm_drop_writes $FAIL; log "device $FAIL drops every write"
	acked=0; : > $T/umltest/fw.acked.$TAG
	while read -r i b fblk; do
		[ $i -lt $FW_REGIONS ] || continue
		dd if=$BB of=$MNT/nocow bs=4096 seek=$b count=1 oflag=direct conv=notrunc \
		   status=none 2>/dev/null && { acked=$((acked + 1)); echo $b >> $T/umltest/fw.acked.$TAG; }
	done < $T/umltest/fw.targets.$TAG
	commits1=$(awk '$1 == "commits" {print $2}' $CS 2>/dev/null)
	log "in-place overwrites: $acked of $FW_REGIONS acknowledged (commits $commits0 -> $commits1)"
	stale0=$(hv device_write_failed)
	dm_error_writes $FAIL; log "device $FAIL fails writes and flushes"
	touch $MNT/marker
	sync
	fl=$(btrfs device stats $MNT 2>/dev/null | awk '/flush_io_errs/ {s += $2} END {print s + 0}')
	named=$(hv stale_marks); torn=$(wv torn_blocks); queued=$(wv repair_queued)
	stale1=$(hv device_write_failed)
	kmsg "did not confirm|write-intent log|raid56:" 6
	dm_heal $FAIL; log "healed device $FAIL"
	touch $MNT/marker2
	sync
	log "FW after the readd: stale_marks=$named torn_blocks=$torn repair_queued=$queued" \
	    "device_write_failed $stale0 -> $stale1 log_flush_unnamed=$(hv log_flush_unnamed)" \
	    "sticky_blocks=$(wv sticky_blocks)"
	# Until every repair asked for has run: they retry on a doubling delay
	# while the device still failed.
	t=0
	while [ $t -lt ${FW_REPAIR_SECS:-90} ]; do
		sleep 1; t=$((t + 1))
		[ $t -ge 3 ] && [ "$(wv repair_queued)" = \
		  "$(( $(wv repair_ok) + $(wv repair_failed) + $(wv repair_skipped) ))" ] && break
	done
	log "FW repairs after ${t}s: queued=$(wv repair_queued) ok=$(wv repair_ok)" \
	    "failed=$(wv repair_failed) dropped=$(wv repair_dropped) gave_up=$(hv repair_gave_up)" \
	    "sticky_blocks=$(wv sticky_blocks) stale_marks=$(hv stale_marks)"
	if [ "$PLAN" = busy ]; then
		# Rows past the targets' whose full stripe lies in one region
		# and whose parity FAIL holds.
		after=$(awk -v n=$((FW_REGIONS + FW_FRESH - 1)) '$1 == n {print $3}' \
			$T/umltest/fw.targets.$TAG)
		awk -v f=$FAIL -v a=${after:-0} '$2 > a && $3 >= 0 && $4 == f && $5 ~ /:0$/ {print $2}' \
			$R | head -n $FW_BUSY > $T/umltest/fw.busy.$TAG
		n=$(wc -l < $T/umltest/fw.busy.$TAG)
		[ "$n" = "$FW_BUSY" ] || { log "LAYOUT_FAIL busy rows $n of $FW_BUSY"; finish; }
		ev0=$(wv sticky_evicted); dr0=$(hv record_dropped); tb=$(wv torn_blocks)
		dmsetup suspend --nolockfs --noflush d$FAIL || log "DM_SUSPEND_FAIL"
		log "device $FAIL suspended, $FW_BUSY writes at once (torn_blocks $tb)"
		start=$(date +%s); pids=""
		rm -f $T/umltest/fw.bres.$TAG.*
		while read -r fb; do
			( dd if=$BB of=$MNT/nocow bs=4096 seek=$fb count=1 oflag=direct \
			     conv=notrunc status=none 2>/dev/null && echo ok || echo eio ) \
				> $T/umltest/fw.bres.$TAG.$fb &
			pids="$pids $!"
		done < $T/umltest/fw.busy.$TAG
		sleep ${FW_HOLD_SECS:-4}
		evh=$(wv sticky_evicted)
		dmsetup resume d$FAIL || log "DM_RESUME_FAIL"
		log "device $FAIL resumed"
		# Not a bare wait: the watchdog is a child too.
		wait $pids
		secs=$(( $(date +%s) - start ))
		bok=$(cat $T/umltest/fw.bres.$TAG.* | grep -c '^ok$')
		beio=$(cat $T/umltest/fw.bres.$TAG.* | grep -c '^eio$')
		ev1=$(wv sticky_evicted); dr1=$(hv record_dropped)
		kmsg "write-intent log full|dropping the record|raid56:" 6
		log "FW busy ok=$bok eio=$beio in ${secs}s, sticky_evicted $ev0 -> $evh (held)" \
		    "-> $ev1, record_dropped $dr0 -> $dr1, torn_blocks $tb -> $(wv torn_blocks)"
		echo "$bok $beio $((evh - ev0)) $((ev1 - ev0)) $((dr1 - dr0)) $tb $secs" \
			> $T/umltest/fw.busy.res.$TAG
		echo "$acked $fl ${named:-0} ${torn:-0} 0 0 $bok $beio 0 0 0 0 $(hv log_full)" \
		     "$(hv record_dropped) $([ "$commits0" = "$commits1" ] && echo 0 || echo 1) $secs" \
			> $T/umltest/fw.$TAG
		rm -f $BB $T/umltest/fw.bres.$TAG.*
		umount $MNT || log "UMOUNT_FAIL"
		dmsetup remove_all 2>/dev/null
		finish
	fi
	full0=$(hv log_full)
	fok=0; feio=0; start=$(date +%s)
	while read -r i b fblk; do
		[ $i -ge $FW_REGIONS ] && [ $i -lt $((FW_REGIONS + FW_FRESH)) ] || continue
		if dd if=$BB of=$MNT/nocow bs=4096 seek=$fblk count=1 oflag=direct conv=notrunc \
		      status=none 2>/dev/null; then
			fok=$((fok + 1))
		else
			feio=$((feio + 1))
		fi
	done < $T/umltest/fw.targets.$TAG
	secs=$(( $(date +%s) - start ))
	sync
	# Every overwritten block, cold.
	echo 3 > /proc/sys/vm/drop_caches
	rok=0; reio=0; rbad=0
	while read -r b; do
		if dd if=$MNT/nocow of=$T/umltest/blk.$TAG bs=4096 skip=$b count=1 status=none \
		      iflag=direct 2>/dev/null; then
			[ "$(tr -cd B < $T/umltest/blk.$TAG | wc -c)" = 4096 ] &&
				rok=$((rok + 1)) || { rbad=$((rbad + 1)); log "FW_READ_BAD $b"; }
		else
			reio=$((reio + 1))
		fi
	done < $T/umltest/fw.acked.$TAG
	stats "end"
	kmsg "write-intent log full|dropping the record|btrfs scrub start|raid56:" 8
	log "FW fresh ok=$fok eio=$feio in ${secs}s, read back ok=$rok eio=$reio bad=$rbad of $acked," \
	    "log_full $full0 -> $(hv log_full) record_dropped=$(hv record_dropped)" \
	    "sticky_evicted=$(wv sticky_evicted) stale_evicted=$(wv stale_evicted)" \
	    "state=$(hv state) action=$(hv action)"
	echo "$acked $fl ${named:-0} ${torn:-0} ${queued:-0} $(wv repair_ok) $fok $feio $rok $reio" \
	     "$rbad $(( ${stale1:-0} - ${stale0:-0} )) $(hv log_full) $(hv record_dropped)" \
	     "$([ "$commits0" = "$commits1" ] && echo 0 || echo 1) $secs" > $T/umltest/fw.$TAG
	rm -f $BB $T/umltest/blk.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
verdict_keep)
	# Does the recovery's verdict on a torn stripe it cannot decide stay the
	# verdict, and keep its place in a full log?  The disks torn_readd_prep's
	# upgrade arm left (CONTROL=2 there: every error record possibly torn),
	# device OMITTED left out, mounted degraded read-write: the recovery
	# records ~70 stripes undecidable, every present parity stale
	# (@suspect_par), in a narrow log of ~97 regions.  Then, by ARM:
	#   replace  (one boot) device REPLACE is replaced by the spare device
	#            TGT, which finishes
	#   remount  PHASE=wide: a few writes into new regions, one of which makes
	#            the log block wide (it names the missing column), and a clean
	#            unmount; PHASE=read: mounted again, the recovery reloads the
	#            verdicts from the wide block as stale parities
	#   flush    as remount, and in PHASE=read (device-mapper over the devices
	#            that are there, no transaction commit) device 0 fails its
	#            flushes while a block is written into the full stripe after
	#            each row's, in the row's region, and finishes; a write into a
	#            new region then makes the log flush, which fails, and the
	#            readd takes those blocks back -- with the stale records the
	#            last block carried for the same regions, the verdicts among
	#            them.  Device 0 is healed.
	# followed by VK_WRITES single-block writes into new regions (the
	# nodatacow file the prep preallocated with VK_SPARE set, one block per
	# 4 MiB), each of which makes the full log spend a record, and the rows are read back as in
	# torn_readd_verify (tr_score).  Fixed, the verdicts stay: the writes spend
	# other records, and the missing column's rows read ok or EIO.
	# CONTROL=1: raid56_wf_replace_end_clears_verdicts=1 (replace) --
	# the replace's end turns the verdicts into ordinary stale parities, the
	# log no longer fits a block ("block full"), writes fail until records
	# are spent, and those spent are verdicts; or
	# raid56_wf_reload_verdicts_plain=1 (remount) -- the reloaded verdicts
	# are spent in table order; or raid56_wf_readd_disowns_all=1 (flush) --
	# the readd takes the verdicts it carries back for its own, and they
	# are spent in table order.  Either way rows read back wrong.
	watchdog ${WATCH:-1500}
	knob=""
	case "$ARM" in
	replace) knob=raid56_wf_replace_end_clears_verdicts;;
	remount) [ "$PHASE" = read ] && knob=raid56_wf_reload_verdicts_plain;;
	flush) [ "$PHASE" = read ] && knob=raid56_wf_readd_disowns_all;;
	esac
	[ "${CONTROL:-0}" = 1 ] && [ -n "$knob" ] && {
		echo 1 > /sys/module/btrfs/parameters/$knob 2>/dev/null || log "CONTROL_KNOB_FAIL"
		log "control: $knob=1"
	}
	if [ "$ARM:$PHASE" = flush:read ]; then
		dm_setup
		dm_scan
		do_mount $OPTS,degraded /dev/mapper/d0
	else
		do_mount $OPTS,degraded $MNTDEV
	fi
	allow_nodatacow
	W=$(ls /sys/fs/btrfs/*-*-*/raid56_write_intent 2>/dev/null | head -1)
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	wv() { awk -v k=$1 '$1 == k {print $2}' $W 2>/dev/null; }
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	kmsg "write-intent log: recovery|does not mark the writes" 2
	log "VK mounted: recovery_suspect=$(wv recovery_suspect)" \
	    "sticky_blocks=$(wv sticky_blocks) torn_blocks=$(wv torn_blocks)" \
	    "sticky_evicted=$(wv sticky_evicted)"
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'C' > /tmp/cblock.vk
	# One 4 KiB block of 'C' at each of the 4 KiB file blocks $3, $3 + $4,
	# ... ($1 of them) of file $2, $5 at a time (default one): echoes the
	# number of writes that failed.
	vk_writes() {
		local n=$1 f=$MNT/$2 par=${5:-1} i
		: > /tmp/vk.bad
		for i in $(seq 0 $((n - 1))); do
			{ dd if=/tmp/cblock.vk of=$f bs=4096 seek=$(($3 + i * $4)) count=1 \
			     oflag=direct conv=notrunc status=none 2>/dev/null ||
				echo $i >> /tmp/vk.bad; } &
			[ $(( (i + 1) % par )) = 0 ] && wait
		done
		wait
		wc -l < /tmp/vk.bad
	}
	failed=0
	case "$ARM:$PHASE" in
	replace:*)
		btrfs replace start -B -K -f $REPLACE $TGT $MNT > /tmp/rep.out 2>&1
		rrc=$?
		log "replace rc=$rrc: $(tr '\n' ' ' < /tmp/rep.out | cut -c1-200)"
		log "replace status:" \
		    "$(btrfs replace status -1 $MNT 2>&1 | tr '\n' ' ' | cut -c1-160)"
		log "VK after the replace: sticky_blocks=$(wv sticky_blocks)" \
		    "sticky_evicted=$(wv sticky_evicted) state=$(hv state)"
		;;
	flush:read)
		# Device 0 fails every flush: its last 4 MiB, which nothing
		# uses, fails every write.  No transaction commit meanwhile
		# (commit=600): its barrier would fail on it too, with device
		# OMITTED already missing, and abort.
		sz=$(blockdev --getsize64 /dev/mapper/d0)
		KM=/tmp/vk.kmsg
		stdbuf -oL dmesg -w > $KM 2>/dev/null &
		kpid=$!
		dm_error_writes_range 0 $((sz - 8388608)) 4194304
		# The full stripe after each row's: another stripe of its
		# region, which no row is read from.
		tfailed=$(vk_writes ${TR_REGIONS:-96} nocow $((NOCOW_FS_BLOCKS)) $TR_STRIDE)
		# A new region: the log has to flush to take it.
		trig=$(vk_writes 1 spare $(( ${VK_WRITES:-40} * 1024 )) 1024)
		fl=$(grep -c "write-intent log flush failed" $KM)
		kill $kpid 2>/dev/null; wait $kpid 2>/dev/null
		dm_heal 0
		kmsg "did not confirm|keeping|not waiting" 4
		log "VK flush: writes into the rows' regions failed $tfailed, the new region's" \
		    "$trig; log flushes failed $fl; sticky_blocks=$(wv sticky_blocks)" \
		    "sticky_evicted=$(wv sticky_evicted) state=$(hv state)"
		echo "$tfailed $trig $fl" > $T/umltest/vk.$TAG.flush
		rm -f $KM
		;;
	remount:wide|flush:wide)
		# Halfway between rows: regions the log lists already, full
		# stripes no row is read from.
		failed=$(vk_writes 8 nocow $((TR_STRIDE / 2)) $TR_STRIDE)
		sync
		kmsg "write-intent log full|block full|lazy commit" 4
		log "VK wide: writes failed $failed, sticky_blocks=$(wv sticky_blocks)" \
		    "sticky_evicted=$(wv sticky_evicted) stale_marks=$(hv stale_marks)"
		echo "$failed $(wv sticky_evicted) $(hv stale_marks)" > $T/umltest/vk.$TAG.wide
		umount $MNT || log "UMOUNT_FAIL"
		finish
		;;
	esac
	rrc=${rrc:-0}
	# After the remount, eight at a time: a record a write of the degraded
	# mount makes becomes the next to spend once the write is done, so
	# writes one after the other spend one old record between them.
	par=1; [ "$ARM" = replace ] || par=8
	failed=$(vk_writes ${VK_WRITES:-40} spare 0 1024 $par)
	sync
	ev=$(wv sticky_evicted)
	full=$(dmesg | grep -c "block full, keeping the previous one")
	kmsg "write-intent log full|block full|lazy commit|still full|could not be decided" 6
	log "VK writes: $failed of ${VK_WRITES:-40} failed, sticky_evicted=$ev block_full=$full" \
	    "sticky_blocks=$(wv sticky_blocks)"
	tr_score
	log "VK phase $PHASE: checked=$checked ok=$ok eio=$eio wrong=$wrong fail_old=$fail_old" \
	    "direct_bad=$direct_bad recovery_suspect=$(wv recovery_suspect)"
	echo "$checked $ok $eio $wrong $direct_bad $failed $full $ev $rrc" \
		> $T/umltest/vk.$TAG.$PHASE
	umount $MNT || log "UMOUNT_FAIL"
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
degraded_crash_prep)
	# A crash while degraded, whose torn stripe has a data column on the
	# missing device.  Boot 1, every device present: a nodatacow file of
	# 'A's (no checksums), and where its first whole full stripe lives --
	# column B (the one whose device the host then leaves out), column C
	# (written by the torn write) and P.  See degraded_crash.sh.
	#
	# PRE=<KiB>: first a nodatacow file of that many 'P's, so that its full
	# stripes come before the torn one (recover_interrupt.sh).
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	if [ -n "${PRE:-}" ]; then
		touch $MNT/pre; chattr +C $MNT/pre || log "CHATTR_FAIL"
		dd if=/dev/zero bs=1K count=$PRE status=none | tr '\000' 'P' > $MNT/pre
		sync
	fi
	touch $MNT/nocow; chattr +C $MNT/nocow || log "CHATTR_FAIL"
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	L=$(python3 $T/umltest/raid56_layout.py $MNT/nocow /dev/mapper/d0 2>&1)
	log "layout: $L"
	echo "$L" > $T/umltest/layout.$TAG
	eval "$L"
	[ -n "${FO_B:-}" ] || log "LAYOUT_FAIL"
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
degraded_crash)
	# Boot 2, column B's device left out by the host: mounted degraded, one
	# block of column C (same row as B's first block) overwritten in place
	# with raid56_crash_point=$CRASH armed.  With 1 the new C lands and the
	# parity does not, and the kernel panics: the parity now describes the
	# old C, and B "rebuilt" from it is B xor the change.
	#
	# NAMED=1: before that, B's first block itself is overwritten with 'N's,
	# fsync'd and committed.  B's device is gone, so the write "fails" there
	# and is acknowledged: the log names column B stale and keeps the
	# record, and the 'N's exist only in the parity -- which the torn write
	# then leaves describing the old C.  The usual shape of a stripe written
	# into while degraded, and one whose record names the missing column.
	#
	# PRE=<KiB>: before that, the 'P' file is rewritten in place with 'Q's,
	# fsync'd: every full stripe of it has a member on the missing device,
	# so the log keeps a record of each, in front of the torn stripe.
	eval "$(cat $T/umltest/layout.$TAG)"
	do_mount $OPTS,degraded $MNTDEV
	allow_nodatacow
	if [ -n "${PRE:-}" ]; then
		dd if=/dev/zero bs=1K count=$PRE status=none | tr '\000' 'Q' |
			dd of=$MNT/pre bs=64K conv=notrunc,fsync status=none 2>/dev/null ||
			log "PRE_WRITE_FAIL"
		sync
		stats pre
	fi
	if [ "${NAMED:-0}" = 1 ]; then
		if dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'N' |
		   dd of=$MNT/nocow bs=4096 seek=$((FO_B / 4096)) count=1 \
		      conv=notrunc,fsync status=none 2>/dev/null; then
			log "named write acknowledged"
		else
			log "NAMED_WRITE_FAIL"
		fi
		sync
		stats named
	fi
	echo ${CRASH:-1} > /sys/module/btrfs/parameters/raid56_crash_point ||
		log "CRASH_ARM_FAIL"
	log "crash armed"
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'C' |
		dd of=$MNT/nocow bs=4096 seek=$((FO_C / 4096)) count=1 conv=notrunc,fsync \
		status=none 2>/dev/null
	log "NO_CRASH"
	finish
	;;
degraded_crash_read)
	# Boot 3 (PHASE=1, column B's device still out) and, on RAID6, boot 4
	# (PHASE=2, P's device out as well): a degraded read-write mount, whose
	# recovery meets the torn stripe, then column B's sixteen blocks of it
	# read back.  Each must be 'A's (block 0 'N's with NAMED=1) or fail;
	# anything else is a rebuild nothing checked, handed back as data.
	#
	# PHASE=ro, before those, with OPTS=ro: the same reads on a read-only
	# degraded mount, which recovers nothing -- the first thing anyone
	# salvaging data off the array would do.  The torn stripe is exactly as
	# the crash left it, and still is for the phases after.
	eval "$(cat $T/umltest/layout.$TAG)"
	do_mount $OPTS,degraded $MNTDEV
	kmsg "write-intent|scrub: full stripe|crash injection" 8
	stats
	echo 3 > /proc/sys/vm/drop_caches
	ok=0; eio=0; wrong=0
	for b in $(seq 0 15); do
		want=A
		[ "${NAMED:-0}" = 1 ] && [ "$b" = 0 ] && want=N
		if dd if=$MNT/nocow of=$T/umltest/blk.$TAG bs=4096 count=1 \
		      skip=$((FO_B / 4096 + b)) status=none 2>/dev/null; then
			n=$(tr -cd "$want" < $T/umltest/blk.$TAG | wc -c)
			if [ "$n" = 4096 ]; then
				ok=$((ok+1))
			else
				wrong=$((wrong+1))
				log "WRONG block $b of column B: $n of 4096 bytes '$want', no error"
			fi
		else
			eio=$((eio+1))
		fi
	done
	rm -f $T/umltest/blk.$TAG
	sleep 2	# the alert work runs asynchronously
	sus=$(sed -n 's/^recovery_suspect //p' /sys/fs/btrfs/*/raid56_write_intent | head -1)
	amb=$(sed -n 's/^read_unverifiable //p' /sys/fs/btrfs/*/raid56_health | head -1)
	unrec=$(sed -n 's/^read_unrecovered //p' /sys/fs/btrfs/*/raid56_health | head -1)
	hstate=$(sed -n 's/^state //p' /sys/fs/btrfs/*/raid56_health | head -1)
	haction=$(sed -n 's/^action //p' /sys/fs/btrfs/*/raid56_health | head -1)
	log "health: $(tr '\n' ' ' < /sys/fs/btrfs/*/raid56_health)"
	kmsg "REFUSED a read|refusing a read|disagree|does not match the Q syndrome" 4
	log "DCR phase ${PHASE:-1}: ok=$ok eio=$eio wrong=$wrong" \
	    "recovery_suspect=${sus:-?} read_unverifiable=${amb:-?}" \
	    "read_unrecovered=${unrec:-?} health=${hstate:-?} action=${haction:-?}"
	echo "$ok $eio $wrong ${sus:-0} ${amb:-0}" > $T/umltest/dcr.$TAG.${PHASE:-1}
	echo "${unrec:-0} ${amb:-0} ${hstate:-?} ${haction:-?}" > $T/umltest/dcrh.$TAG.${PHASE:-1}
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
degraded_csum_prep)
	# See degraded_csum.sh.  Boot 1, every device present: one block of 'S's
	# with a checksum, then two nodatacow files of 'A's (no checksums), each
	# written and committed on its own: small extents follow one another,
	# so the first fills the rest of the checksummed block's data column
	# and the second the next column.  Then where they are
	# (raid56_csum_layout.py): column B holds the checksummed block (row
	# S_ROW) and blocks of bcol in its other rows (their file offsets in
	# B_ROWS), column C a block of nocow in the same row as the checksummed
	# one (FO_C).  The host leaves B's device out of the boots after this
	# one, and degraded_crash tears the write into C's block.  nossd: no
	# allocation cluster.
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS,nossd /dev/mapper/d0
	allow_nodatacow
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'S' > $MNT/csum
	sync
	for f in bcol:15 nocow:16; do
		touch $MNT/${f%:*}; chattr +C $MNT/${f%:*} || log "CHATTR_FAIL"
		dd if=/dev/zero bs=4096 count=${f#*:} status=none | tr '\000' 'A' > $MNT/${f%:*}
		sync
	done
	L=$(python3 $T/umltest/raid56_csum_layout.py $MNT/csum $MNT/bcol $MNT/nocow \
	    /dev/mapper/d0 2>&1)
	log "layout: $L"
	echo "$L" > $T/umltest/layout.$TAG
	case "$L" in FULL=*) ;; *) log "LAYOUT_FAIL";; esac
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
degraded_csum_read)
	# Boot 3, column B's device still out: a degraded read-write mount,
	# whose recovery meets the stripe degraded_crash tore (and the host
	# tore Q in), then column B's blocks of bcol (B_ROWS) and the
	# checksummed block read back.  In bcol's rows neither parity changed,
	# and the read's Q cross-check vouches for the rebuild:
	#   ok     'A'
	#   eio    refused
	#   wrong  anything else, with no error
	# The checksummed block is in the torn row: its checksum refuses every
	# rebuild, so it reads 'S' (ok) or fails (eio); anything else is wrong.
	eval "$(cat $T/umltest/layout.$TAG)"
	log "knob raid56_recover_absent_checked_suspect=$(cat \
	    /sys/module/btrfs/parameters/raid56_recover_absent_checked_suspect 2>/dev/null ||
	    echo absent)"
	do_mount $OPTS,degraded $MNTDEV
	kmsg "write-intent|scrub: full stripe|crash injection" 8
	stats
	echo 3 > /proc/sys/vm/drop_caches
	ok=0; eio=0; wrong=0
	for o in $B_ROWS; do
		if dd if=$MNT/bcol of=$T/umltest/blk.$TAG bs=4096 count=1 \
		      skip=$((o / 4096)) status=none 2>/dev/null; then
			n=$(tr -cd A < $T/umltest/blk.$TAG | wc -c)
			if [ "$n" = 4096 ]; then
				ok=$((ok+1))
			else
				wrong=$((wrong+1))
				log "WRONG block at $o of bcol (column B): $n of 4096 'A', no error"
			fi
		else
			eio=$((eio+1))
		fi
	done
	if dd if=$MNT/csum of=$T/umltest/blk.$TAG bs=4096 count=1 status=none 2>/dev/null; then
		cs=ok
		[ "$(tr -cd S < $T/umltest/blk.$TAG | wc -c)" = 4096 ] || {
			cs=wrong; log "WRONG checksummed block, no error"
		}
	else
		cs=eio
	fi
	rm -f $T/umltest/blk.$TAG
	sleep 2	# the alert work runs asynchronously
	sus=$(sed -n 's/^recovery_suspect //p' /sys/fs/btrfs/*/raid56_write_intent | head -1)
	und=$(sed -n 's/^torn_undecidable //p' /sys/fs/btrfs/*/raid56_health | head -1)
	kmsg "match neither|parities disagree|REFUSED a read|refusing a read" 4
	log "DCS ok=$ok eio=$eio wrong=$wrong csum=$cs recovery_suspect=${sus:-?}" \
	    "torn_undecidable=${und:-?}"
	echo "$ok $eio $wrong $cs ${sus:-?} ${und:-?}" > $T/umltest/dcs.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
torn_present_prep)
	# See torn_present.sh.  Boot 1, every device present: a nodatacow file
	# of 'A's (no checksums), and where its first whole full stripe lives
	# (raid56_layout.py: FO_B, FO_C, FO_D the file offsets of data columns
	# 0, 1 and 2, IDX_* and PHYS_* the devices and physical offsets of
	# those columns and of P and Q).
	dm_setup
	mkfs.btrfs -K -q -f -d $DPROF -m $MPROF $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	touch $MNT/nocow; chattr +C $MNT/nocow || log "CHATTR_FAIL"
	dd if=/dev/zero bs=1M count=2 status=none | tr '\000' 'A' > $MNT/nocow
	sync
	L=$(python3 $T/umltest/raid56_layout.py $MNT/nocow /dev/mapper/d0 2>&1)
	log "layout: $L"
	echo "$L" > $T/umltest/layout.$TAG
	eval "$L"
	[ -n "${FO_B:-}" ] || log "LAYOUT_FAIL"
	umount $MNT || log "UMOUNT_FAIL"
	dmsetup remove_all 2>/dev/null
	finish
	;;
torn_present_fault)
	# Boot 2, every device present, device-mapper over them.
	#   FAILDEV=<B|C|D> that column's device fails the writes into its
	#                 column of the stripe (and its flushes) while block WROW
	#                 of column WRITECOL is overwritten with 'N's, fsync'd:
	#                 acknowledged within the parity's tolerance, and the log
	#                 names what it can -- a data column stale, the 'N's
	#                 only in the parity -- and keeps the record (the repair
	#                 is held off).  Then the device is healed.
	#   CRASH=<1|2>   then block ROW of column TORN is overwritten in place
	#                 with 'C's with raid56_crash_point armed: with 1 the new
	#                 data lands, the parity does not, and the kernel panics.
	eval "$(cat $T/umltest/layout.$TAG)"
	watchdog 600
	dm_setup
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	echo 600000 > /sys/module/btrfs/parameters/raid56_repair_delay_ms
	if [ -n "${FAILDEV:-}" ]; then
		eval "fidx=\$IDX_$FAILDEV fphys=\$PHYS_$FAILDEV wfo=\$FO_$WRITECOL"
		dm_error_writes_range $fidx $fphys 65536
		log "device $fidx fails the writes into member $FAILDEV of the stripe"
		if dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'N' |
		   dd of=$MNT/nocow bs=4096 seek=$((wfo / 4096 + ${WROW:-0})) count=1 \
		      conv=notrunc,fsync status=none 2>/dev/null; then
			log "named write acknowledged"
		else
			log "NAMED_WRITE_FAIL"
		fi
		sync
		stats named
		log "named health: $(tr '\n' ' ' < /sys/fs/btrfs/*-*-*/raid56_health)"
		dm_heal $fidx; log "healed device $fidx"
		sync
	fi
	log "before the crash: $(tr '\n' ' ' < /sys/fs/btrfs/*-*-*/raid56_health)"
	eval "tfo=\$FO_$TORN"
	echo $CRASH > /sys/module/btrfs/parameters/raid56_crash_point ||
		log "CRASH_ARM_FAIL"
	log "crash armed"
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'C' |
		dd of=$MNT/nocow bs=4096 seek=$((tfo / 4096 + ROW)) count=1 conv=notrunc,fsync \
		status=none 2>/dev/null
	log "NO_CRASH"
	finish
	;;
torn_present_read)
	# Boot 3 on: mounted $OPTS, degraded when the host left a device out;
	# BAD=<col> first makes block ROW of that column unreadable
	# (device-mapper over the devices there are), or with BADAFTER=1 only
	# once the mount -- and so its recovery -- is done.
	#   NAMEBY=<flush|write>  with BAD: once the recovery has kept the
	#                 stripe torn, BAD's block reads again, and block WROW of
	#                 column NAMECOL is overwritten with 'N's (O_DIRECT, no
	#                 fsync), acknowledged; its device then loses it the only
	#                 ways the rig can say so -- flush: it fails the barrier
	#                 of the next commit, so the readd names the column and
	#                 asks for a repair (wib_readd_queue_repairs()); write: it
	#                 fails the write itself, which names the column and
	#                 queues a repair on the fault.  The device is healed and
	#                 the repair given NAME_WAIT seconds to run.
	#   FILL=<n>      with BAD, BAD's block left unreadable: once the
	#                 recovery has kept the stripe torn, device FILLDEV
	#                 throws its writes away while one block is overwritten
	#                 in each of n regions of a second nodatacow file, where
	#                 FILLDEV's member gets the write (as flush_wedge picks
	#                 them), then fails a commit's barrier: the readd takes
	#                 them back possibly torn, too many to name, and with the
	#                 recovery's record the log is full of records that say
	#                 no more than that (n = 164: a narrow block holds 165).
	#                 FILLDEV is healed and one write goes into a new
	#                 region, with nothing in flight: it spends one of them.
	# The sixteen blocks of each column in READCOLS of the stripe are read
	# back:
	#   ok     what was acknowledged: 'N' where the fault boot wrote them, 'A'
	#          elsewhere, 'A' or 'C' for the block the crash tore
	#   eio    the read failed: refused, not wrong
	#   wrong  anything else, handed back as data
	eval "$(cat $T/umltest/layout.$TAG)"
	if [ -n "${BAD:-}" ]; then
		dm_setup
		eval "bidx=\$IDX_$BAD bphys=\$PHYS_$BAD"
		# Device-mapper numbers the devices there are.
		bdm=$bidx
		[ -n "${OMITTED:-}" ] && [ "$OMITTED" -lt "$bidx" ] && bdm=$((bidx - 1))
		if [ "${BADAFTER:-0}" != 1 ]; then
			dm_bad_sectors $bdm $((bphys + ROW * 4096))
			log "block $ROW of column $BAD unreadable on device $bidx"
		fi
		dm_scan
		do_mount $OPTS${OMITTED:+,degraded} /dev/mapper/d0
		if [ "${BADAFTER:-0}" = 1 ]; then
			dm_bad_sectors $bdm $((bphys + ROW * 4096))
			log "block $ROW of column $BAD unreadable on device $bidx, after the mount"
		fi
	else
		do_mount $OPTS${OMITTED:+,degraded} $MNTDEV
	fi
	kmsg "write-intent|scrub|possibly torn|crash injection|raid56" 20
	stats
	fillout=""
	if [ -n "${BAD:-}" ] && [ -n "${FILL:-}" ]; then
		allow_nodatacow
		eval "fdev=\$IDX_$FILLDEV"
		W=$(ls /sys/fs/btrfs/*-*-*/raid56_write_intent 2>/dev/null | head -1)
		wv() { awk -v k=$1 '$1 == k {print $2}' $W 2>/dev/null; }
		# 35 full stripes apart: more than a region (4 MiB) on three
		# devices, and not a multiple of three, so the columns rotate.
		stride=$(( 35 * NOCOW_FS_BLOCKS )); nrows=$((FILL + 30))
		touch $MNT/fill; chattr +C $MNT/fill || log "CHATTR_FAIL"
		fallocate -l $(( (nrows + 1) * stride * 4096 )) $MNT/fill ||
			{ log "FALLOCATE_FAIL"; finish; }
		sync
		R=$T/umltest/tp.rows.$TAG
		python3 $T/umltest/raid56_rows.py $MNT/fill /dev/mapper/d0 $stride $nrows region \
			> $R 2>&1
		k=0
		while read -r i fblk col pdev reg mates; do
			[ "$col" -ge 0 ] 2>/dev/null && [ "${reg##*:}" = 0 ] || continue
			b=-1
			if [ "$pdev" = "$fdev" ]; then
				b=$fblk
			else
				for m in $mates; do [ "${m##*:}" = "$fdev" ] && b=${m%%:*}; done
			fi
			[ "$b" -ge 0 ] || continue
			echo "$k $b $fblk"; k=$((k + 1))
		done < $R > $T/umltest/tp.targets.$TAG
		n=$(wc -l < $T/umltest/tp.targets.$TAG)
		[ "$n" -gt "$FILL" ] || { log "LAYOUT_FAIL $n of $((FILL + 1)) fill rows"; finish; }
		FB=$T/umltest/fblock.$TAG
		dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'F' > $FB
		dm_drop_writes $fdev; log "device $fdev drops every write"
		while read -r i b fblk; do
			[ $i -lt $FILL ] || continue
			dd if=$FB of=$MNT/fill bs=4096 seek=$b count=1 oflag=direct conv=notrunc \
			   status=none 2>/dev/null || log "TP_FILL_FAIL $i"
		done < $T/umltest/tp.targets.$TAG
		dm_error_writes $fdev; log "device $fdev fails writes and flushes"
		touch $MNT/marker
		sync
		dm_heal $fdev; log "healed device $fdev"
		touch $MNT/marker2
		sync
		ev0=$(wv sticky_evicted); tb=$(wv torn_blocks)
		fb=$(awk -v n=$FILL '$1 == n {print $3}' $T/umltest/tp.targets.$TAG)
		dd if=$FB of=$MNT/fill bs=4096 seek=$fb count=1 oflag=direct conv=notrunc \
		   status=none 2>/dev/null || log "TP_FRESH_FAIL"
		sp=$(( $(wv sticky_evicted) - ev0 ))
		log "TP fill: torn_blocks $tb, the write into a new region spent $sp record(s)"
		kmsg "did not confirm|dropping the record|write-intent log full" 6
		fillout=" tb=$tb spent=$sp"
		rm -f $FB
	fi
	if [ -n "${BAD:-}" ] && [ -n "${NAMEBY:-}" ]; then
		allow_nodatacow
		dm_heal $bidx; log "block $ROW of column $BAD reads again"
		eval "nidx=\$IDX_$NAMECOL nphys=\$PHYS_$NAMECOL nfo=\$FO_$NAMECOL"
		ndev=$(echo $DEVS | awk -v i=$((nidx + 1)) '{print $i}')
		nsz=$(blockdev --getsize64 $ndev)
		[ "$NAMEBY" = write ] && dm_error_writes_range $nidx $nphys 65536
		if dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'N' |
		   dd of=$MNT/nocow bs=4096 seek=$((nfo / 4096 + ${WROW:-0})) count=1 \
		      oflag=direct conv=notrunc status=none 2>/dev/null; then
			log "named write acknowledged"
		else
			log "NAMED_WRITE_FAIL"
		fi
		if [ "$NAMEBY" = flush ]; then
			# 4 MiB near the end, which nothing uses: its writes
			# fail, and so does every flush of the device.
			dm_error_writes_range $nidx $((nsz - 8388608)) 4194304
			touch $MNT/marker
			sync
			log "device $nidx failed the barrier:" \
			    "$(btrfs device stats $MNT 2>/dev/null | awk '/flush_io_errs/ {s += $2} END {print s + 0}')" \
			    "flush errors"
		fi
		dm_heal $nidx; log "healed device $nidx"
		sleep ${NAME_WAIT:-8}
		kmsg "did not confirm|refusing|repair|undecidable" 6
	fi
	echo 3 > /proc/sys/vm/drop_caches
	out=""
	# READCOLS=B_C: the kernel command line it comes on splits at spaces.
	for col in $(echo ${READCOLS:-B} | tr _ ' '); do
		eval "fo=\$FO_$col"
		ok=0; eio=0; wrong=0
		for b in $(seq 0 15); do
			want=A; alt=A
			[ "$col" = "${WRITECOL:-}" ] && [ "$b" = "${WROW:-0}" ] && want=N
			[ "$col" = "${TORN:-}" ] && [ "$b" = "${ROW:-0}" ] && alt=C
			if dd if=$MNT/nocow of=$T/umltest/blk.$TAG bs=4096 count=1 \
			      skip=$((fo / 4096 + b)) status=none 2>/dev/null; then
				n=$(tr -cd "$want" < $T/umltest/blk.$TAG | wc -c)
				m=$(tr -cd "$alt" < $T/umltest/blk.$TAG | wc -c)
				if [ "$n" = 4096 ] || [ "$m" = 4096 ]; then
					ok=$((ok+1))
				else
					wrong=$((wrong+1))
					m=$(tr -cd 'C' < $T/umltest/blk.$TAG | wc -c)
					log "TP_WRONG block $b of column $col:" \
					    "$n of 4096 bytes '$want', $m 'C', no error"
				fi
			else
				eio=$((eio+1))
			fi
		done
		out="$out ${col}_ok=$ok ${col}_eio=$eio ${col}_wrong=$wrong"
	done
	rm -f $T/umltest/blk.$TAG
	sleep 2	# the alert work runs asynchronously
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	sus=$(sed -n 's/^recovery_suspect //p' /sys/fs/btrfs/*/raid56_write_intent | head -1)
	out="$out sus=${sus:-0} unrec=$(hv read_unrecovered) tund=$(hv torn_undecidable)"
	out="$out amb=$(hv read_unverifiable) state=$(hv state) action=$(hv action)"
	out="$out und=$(hv stripe_undecidable) stale=$(hv device_write_failed)$fillout"
	log "health: $(tr '\n' ' ' < $H)"
	kmsg "REFUSED a read|refusing a read|could not decide|cannot be decided|left untouched" 6
	log "TP phase ${PHASE:-1}:$out"
	echo "$out" > $T/umltest/tp.$TAG.${PHASE:-1}
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
torn_present_runtime)
	# Boot 3 of torn_present.sh's runtime scenario, every device present,
	# device-mapper over them.  Block ROW of column B does not read at the
	# mount, so the recovery keeps the stripe the crash tore undecided,
	# marked possibly torn, naming nothing.  Then B's device is healed and
	# fails only the write of B's block 0 ('N's, acknowledged): the log
	# names B stale, the 'N's are only in P -- which may be torn.  Column
	# B's block 0 is read (a rebuild from P alone: refused), the one
	# explanation of the refusal kept, and a scrub run, which cannot
	# decide the stripe; then block 0 is read again.
	eval "$(cat $T/umltest/layout.$TAG)"
	watchdog 600
	dm_setup
	dm_bad_sectors $IDX_B $((PHYS_B + ROW * 4096))
	log "block $ROW of column B unreadable on device $IDX_B"
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	echo 600000 > /sys/module/btrfs/parameters/raid56_repair_delay_ms
	kmsg "write-intent|possibly torn|raid56" 6
	dm_heal $IDX_B
	dm_error_writes_range $IDX_B $PHYS_B 65536
	if dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'N' |
	   dd of=$MNT/nocow bs=4096 seek=$((FO_B / 4096)) count=1 conv=notrunc,fsync \
	      status=none 2>/dev/null; then
		log "named write acknowledged"
	else
		log "NAMED_WRITE_FAIL"
	fi
	dm_heal $IDX_B
	rb0() {	# echoes ok|eio|wrong for column B's block 0
		echo 3 > /proc/sys/vm/drop_caches
		if dd if=$MNT/nocow of=$T/umltest/blk.$TAG bs=4096 count=1 \
		      skip=$((FO_B / 4096)) status=none 2>/dev/null; then
			[ "$(tr -cd N < $T/umltest/blk.$TAG | wc -c)" = 4096 ] &&
				echo ok || echo wrong
		else
			echo eio
		fi
		rm -f $T/umltest/blk.$TAG
	}
	b0=$(rb0)
	sleep 2	# the alert work runs asynchronously
	ex=none
	dmesg | grep -q "REFUSED a read.*the scrub rewrites the parity from the data and the" &&
		ex=promise
	dmesg | grep -q "REFUSED a read.*a scrub leaves the stripe as it is" && ex=undecidable
	kmsg "REFUSED a read" 2
	btrfs scrub start -B $MNT > /tmp/tpr.scrub 2>&1
	log "scrub rc=$?: $(tr '\n' ' ' < /tmp/tpr.scrub | cut -c1-200)"
	left=$(dmesg | grep -c "left untouched: a write into it may have been torn")
	b0s=$(rb0)
	sleep 2
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	hv() { awk -v k=$1 '$1 == k {print $2}' $H 2>/dev/null; }
	kmsg "cannot be decided|left untouched" 3
	log "health: $(tr '\n' ' ' < $H)"
	out="b0=$b0 explain=$ex scrub_left=$left b0_scrubbed=$b0s tund=$(hv torn_undecidable)"
	out="$out unrec=$(hv read_unrecovered) action=$(sed -n 's/^action //p' $H | tr ' ' '_')"
	log "TP phase ${PHASE:-1}: $out"
	echo " $out" > $T/umltest/tp.$TAG.${PHASE:-1}
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
torn_present_clear)
	# Boot 4 of torn_present.sh's clear scenario, every device present:
	# the stripe the named scenario's recovery recorded undecidable (B
	# named, P suspect).  An in-place rewrite of B's block 1 is refused.
	# Then what the alert says to do: delete the file, sync, scrub.  A new
	# nodatacow file is preallocated over the same space and its blocks in
	# column B written in place, a read-modify-write each, and read back.
	eval "$(cat $T/umltest/layout.$TAG)"
	watchdog 600
	dm_setup
	dm_scan
	do_mount $OPTS,nossd /dev/mapper/d0
	allow_nodatacow
	echo 600000 > /sys/module/btrfs/parameters/raid56_repair_delay_ms
	if dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'R' |
	   dd of=$MNT/nocow bs=4096 seek=$((FO_B / 4096 + 1)) count=1 conv=notrunc,fsync \
	      status=none 2>/dev/null; then
		wr1=ok
	else
		wr1=eio
	fi
	sleep 2
	ex=old
	dmesg | grep -q "more of it recorded stale.*delete them, run 'sync'" && ex=new
	kmsg "REFUSED|undecidable|cannot be decided" 3
	H=$(ls /sys/fs/btrfs/*-*-*/raid56_health 2>/dev/null | head -1)
	act=$(sed -n 's/^action //p' $H | tr ' ' '_')
	rm -f $MNT/nocow
	sync
	btrfs scrub start -B $MNT > /tmp/tpc.scrub 2>&1
	log "scrub rc=$?: $(tr '\n' ' ' < /tmp/tpc.scrub | cut -c1-200)"
	log "after the scrub: $(tr '\n' ' ' < $H)"
	touch $MNT/nocow2; chattr +C $MNT/nocow2 || log "CHATTR_FAIL"
	fallocate -l 2M $MNT/nocow2 || log "FALLOCATE_FAIL"
	sync
	L2=$(python3 $T/umltest/raid56_layout.py $MNT/nocow2 /dev/mapper/d0 2>&1)
	log "new layout: $L2"
	full2=$(echo "$L2" | sed -n 's/.*FULL=\([0-9]*\).*/\1/p')
	fob2=$(echo "$L2" | sed -n 's/.*FO_B=\([0-9]*\).*/\1/p')
	same=0
	[ -n "$full2" ] && [ "$full2" = "$FULL" ] && same=1
	wok=0; weio=0; rok=0
	for b in $(seq 0 15); do
		[ -n "$fob2" ] || break
		if dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'R' |
		   dd of=$MNT/nocow2 bs=4096 seek=$((fob2 / 4096 + b)) count=1 \
		      conv=notrunc oflag=direct status=none 2>/dev/null; then
			wok=$((wok+1))
		else
			weio=$((weio+1))
		fi
	done
	sync
	echo 3 > /proc/sys/vm/drop_caches
	for b in $(seq 0 15); do
		[ -n "$fob2" ] || break
		dd if=$MNT/nocow2 of=$T/umltest/blk.$TAG bs=4096 count=1 \
		   skip=$((fob2 / 4096 + b)) status=none 2>/dev/null &&
		[ "$(tr -cd R < $T/umltest/blk.$TAG | wc -c)" = 4096 ] && rok=$((rok+1))
	done
	rm -f $T/umltest/blk.$TAG
	kmsg "refusing a write|REFUSED a write|left untouched" 3
	out="wr1=$wr1 explain=$ex action=$act same=$same w_ok=$wok w_eio=$weio r_ok=$rok"
	log "TP phase ${PHASE:-1}: $out"
	echo " $out" > $T/umltest/tp.$TAG.${PHASE:-1}
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
degraded_crash_kill)
	# Boot 3 of recover_interrupt.sh, column B's device still out and the
	# log listing the 'P' file's stripes in front of the torn one: the
	# read-only degraded mount of degraded_crash_read's PHASE=ro, then
	# made read-write with the recovery lingering $RIK_DELAY ms on each
	# stripe (raid56_recovery_delay_ms).  Column B's sixteen blocks of the
	# torn stripe are read
	#   ro      before the remount: refused, the parity may be torn
	#   window  while the recovery lingers on the first 'P' stripe
	#   killed  after that remount was SIGKILLed -- it fails at the next
	#           stripe, and the mount stays read-only
	#   self    during a second remount, while the recovery lingers
	#           $RIK_SELF_DELAY ms on the torn stripe itself: taken over
	#           from the log, its verdict not in yet
	#   retry   after that remount has run the recovery to the end
	# Each block must read 'A' or fail; nothing about the torn stripe has
	# been checked until the recovery decides it.  CONTROL=1 sets
	# raid56_wf_recover_drops_refusals=1: the recovery drops the refusal
	# before it reaches any stripe and does not put it back, and block 0
	# reads back as the rebuild from the torn parity, with no error.
	# CONTROL=2 sets raid56_wf_recovering_stripe_readable=1: only the
	# stripe the recovery is deciding goes unrefused, and block 0 reads
	# back so in the self phase alone.
	watchdog ${WATCH:-900}
	eval "$(cat $T/umltest/layout.$TAG)"
	do_mount ro,degraded $MNTDEV
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_recover_drops_refusals ||
			log "CONTROL_KNOB_FAIL"
		log "control: the recovery drops the refusals up front"
	}
	[ "${CONTROL:-0}" = 2 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_wf_recovering_stripe_readable ||
			log "CONTROL_KNOB_FAIL"
		log "control: the stripe the recovery is deciding is not refused"
	}
	# No drop_caches while the remount runs: it waits for s_umount.  The
	# log lines go to the console, the counts to the caller.
	readb() {	# phase
		local ok=0 eio=0 wrong=0 b n
		for b in $(seq 0 15); do
			if dd if=$MNT/nocow of=$T/umltest/blk.$TAG bs=4096 count=1 \
			      skip=$((FO_B / 4096 + b)) status=none 2>/dev/null; then
				n=$(tr -cd A < $T/umltest/blk.$TAG | wc -c)
				if [ "$n" = 4096 ]; then
					ok=$((ok+1))
				else
					wrong=$((wrong+1))
					log "WRONG $1: block $b of column B:" \
					    "$n of 4096 bytes 'A', no error" >&2
				fi
			else
				eio=$((eio+1))
			fi
		done
		rm -f $T/umltest/blk.$TAG
		log "RIK $1: ok=$ok eio=$eio wrong=$wrong" >&2
		echo "$ok/$eio/$wrong"
	}
	rd_ro=$(readb ro)
	echo ${RIK_DELAY:-3000} > /sys/module/btrfs/parameters/raid56_recovery_delay_ms ||
		log "DELAY_KNOB_FAIL"
	delays() { dmesg | grep -c "raid56 recovery delay"; }
	n0=$(delays)
	mount -o remount,rw,degraded $MNT > /tmp/rik.out 2>&1 &
	mpid=$!
	lingering=0
	for i in $(seq 1 600); do
		[ "$(delays)" -gt "$n0" ] && { lingering=1; break; }
		kill -0 $mpid 2>/dev/null || break
		sleep 0.1
	done
	log "recovery lingering: $lingering"
	rd_win=$(readb window)
	kill -9 $mpid 2>/dev/null
	wait $mpid
	mrc=$?
	echo 0 > /sys/module/btrfs/parameters/raid56_recovery_delay_ms
	mo=$(awk -v m=$MNT '$2 == m {split($4, o, ","); print o[1]}' /proc/mounts)
	log "remount killed: rc=$mrc, mounted ${mo:-?}: $(tr '\n' ' ' < /tmp/rik.out | cut -c1-160)"
	kmsg "recovery interrupted|recovery stopped|recovery done" 4
	stopped=$(dmesg | grep -c "recovery stopped")
	echo 3 > /proc/sys/vm/drop_caches
	rd_kill=$(readb killed)
	echo 3 > /proc/sys/vm/drop_caches
	echo ${RIK_SELF_DELAY:-6000} > /sys/module/btrfs/parameters/raid56_recovery_delay_ms
	mount -o remount,rw,degraded $MNT > /tmp/rik2.out 2>&1 &
	mpid=$!
	selfl=0
	for i in $(seq 1 1200); do
		dmesg | grep -Eq "recovery delay: .* full stripe $FULL\$" && { selfl=1; break; }
		kill -0 $mpid 2>/dev/null || break
		sleep 0.1
	done
	log "recovery lingering on the torn stripe: $selfl"
	rd_self=$(readb self)
	wait $mpid
	echo 0 > /sys/module/btrfs/parameters/raid56_recovery_delay_ms
	head -3 /tmp/rik2.out
	mo2=$(awk -v m=$MNT '$2 == m {split($4, o, ","); print o[1]}' /proc/mounts)
	log "remounted: ${mo2:-?}"
	echo 3 > /proc/sys/vm/drop_caches
	rd_retry=$(readb retry)
	amb=$(sed -n 's/^read_unverifiable //p' /sys/fs/btrfs/*/raid56_health | head -1)
	unrec=$(sed -n 's/^read_unrecovered //p' /sys/fs/btrfs/*/raid56_health | head -1)
	log "health: $(tr '\n' ' ' < /sys/fs/btrfs/*/raid56_health)"
	log "RIK lingering=$lingering mrc=$mrc mounted=${mo:-?} stopped=$stopped" \
	    "remounted=${mo2:-?} ro=$rd_ro window=$rd_win killed=$rd_kill retry=$rd_retry" \
	    "read_unverifiable=${amb:-?} read_unrecovered=${unrec:-?}" \
	    "self_lingering=$selfl self=$rd_self"
	echo "$lingering $mrc ${mo:-?} $stopped ${mo2:-?} $rd_ro $rd_win $rd_kill" \
	     "$rd_retry ${amb:-0} $selfl $rd_self" > $T/umltest/rik.$TAG
	umount $MNT || log "UMOUNT_FAIL"
	finish
	;;
parity_rotation)
	# Does scrub look for a full stripe's parity where it actually is?
	#
	# RAID6 data over five devices -- three data columns, P and Q per full
	# stripe -- and raid1c3 metadata.  The columns rotate by one device per
	# full stripe: column c of full stripe n is on chunk stripe (c + n) % 5,
	# so P and Q are on chunk stripes 3 and 4 in full stripe 0 only.
	# scrub_raid56_plan_wib() used to count the usable parities there in
	# every full stripe.  That only gives a different answer when a device
	# is missing, so:
	#
	#   M  the device at chunk stripe 3, removed; mounted degraded
	#   W  the device at chunk stripe 0, failing writes while one 4K block
	#      of its data column is overwritten in each full stripe where it
	#      holds one.  Two faults, within RAID6's tolerance: acknowledged,
	#      and W's column recorded stale.  W is healed afterwards.
	#
	# The file is nodatacow, so nothing but the record says those blocks
	# are stale, and the automatic repair is off (raid56_no_repair_on_fault)
	# so that the scrub is what has to act on it.  Every such stripe has at
	# most two columns it cannot believe -- W's, and M's where M holds data
	# -- and the parity to rebuild them, so the scrub must put 'B' back on
	# W's platter everywhere.  Looked up without the rotation, M counts as
	# P in every full stripe, which leaves too little parity wherever M
	# really holds a data column or Q (recorded bad by the degraded write):
	# the stripe is declared ambiguous and left with W's stale block.
	# Full stripe 0 of every five is where both lookups agree; it must be
	# repaired in both arms, or the control shows nothing.
	#
	# CONTROL=1 sets raid56_scrub_parity_unrotated=1.  Every row the layout
	# helper prints says which verdict the kernel should reach with and
	# without the rotation; the driver compares the platter with that.
	watchdog ${WATCH:-900}
	dm_setup
	[ $(echo $DMDEVS | wc -w) = 5 ] || { log "NEED_5_DEVICES"; finish; }
	mkfs.btrfs -K -q -f -d raid6 -m raid1c3 $DMDEVS || { log "MKFS_FAIL"; finish; }
	dm_scan
	do_mount $OPTS /dev/mapper/d0
	allow_nodatacow
	echo 1 > /sys/module/btrfs/parameters/raid56_no_repair_on_fault 2>/dev/null ||
		log "NOREPAIR_KNOB_FAIL"
	[ "${CONTROL:-0}" = 1 ] && {
		echo 1 > /sys/module/btrfs/parameters/raid56_scrub_parity_unrotated 2>/dev/null ||
			log "CONTROL_KNOB_FAIL"
		log "control: parities looked up without the rotation"
	}
	touch $MNT/nocow; chattr +C $MNT/nocow || { log "CHATTR_FAIL"; finish; }
	# Whole full stripes (3 x 64 KiB), so that no full stripe the scrub
	# visits is only partly this file: M's column of every one of them is
	# rebuilt with nothing to check it against, and a partial one can end
	# the device's scrub early rather than say anything about this.
	dd if=/dev/zero bs=64K count=$(( ${PR_ROWS:-20} * 3 )) status=none | tr '\000' 'A' > $MNT/nocow
	sync
	LAY=$T/umltest/prot.layout.$TAG
	# First line: M, W (dm indices), ND, NUM, ALIGNED.  Then one line per
	# full stripe where W holds a data column: "ROW n file_offset
	# physical_on_W verdict_rotated verdict_unrotated", the verdict being
	# what scrub_raid56_plan_wib() decides for it once M is missing, W's
	# column recorded stale, and M's parity (where it holds one) recorded
	# bad by the degraded write.
	python3 - $MNT/nocow /dev/mapper/d0 > $LAY 2>&1 <<'PY'
import re, subprocess, sys
path, dev = sys.argv[1:3]
SL = 65536
ext = []
for line in subprocess.run(['filefrag', '-v', '-b4096', path],
                           capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'\s*\d+:\s+(\d+)\.\.\s*(\d+):\s+(\d+)\.\.\s*(\d+):', line)
    if m:
        f0, f1, p0, _ = (int(x) * 4096 for x in m.groups())
        if ext and ext[-1][1] == f0 and ext[-1][2] + (ext[-1][1] - ext[-1][0]) == p0:
            ext[-1] = (ext[-1][0], f1 + 4096, ext[-1][2])
        else:
            ext.append((f0, f1 + 4096, p0))
if not ext:
    sys.exit('no extents')
out = subprocess.run(['btrfs', 'inspect-internal', 'dump-tree', '-t', 'chunk', dev],
                     capture_output=True, text=True).stdout
chunks, cur = [], None
for line in out.splitlines():
    m = re.search(r'CHUNK_ITEM (\d+)\)', line)
    if m:
        cur = {'start': int(m.group(1)), 'stripes': []}
        chunks.append(cur)
        continue
    if cur is None:
        continue
    m = re.search(r'length (\d+) owner \d+ stripe_len (\d+) type (\S+)', line)
    if m:
        cur['len'], cur['type'] = int(m.group(1)), m.group(3)
    m = re.search(r'stripe (\d+) devid (\d+) offset (\d+)', line)
    if m:
        cur['stripes'].append((int(m.group(2)), int(m.group(3))))
devpath = {}
show = subprocess.run(['btrfs', 'filesystem', 'show', dev], capture_output=True,
                      text=True).stdout
for m in re.finditer(r'devid\s+(\d+)\s+size.*?path\s+(\S+)', show):
    devpath[int(m.group(1))] = m.group(2)
c = next((c for c in chunks if 'len' in c and
          c['start'] <= ext[0][2] < c['start'] + c['len']), None)
if c is None or 'RAID6' not in c['type'] or len(c['stripes']) != 5:
    sys.exit('file not in a five-device RAID6 chunk')
num = len(c['stripes'])
nd = num - 2
fsl = nd * SL
iM, iW = nd, 0
def dmidx(k):
    return int(re.search(r'(\d+)$', devpath[c['stripes'][k][0]]).group(1))
def verdict(n, rotated):
    # scrub_raid56_plan_wib() for this full stripe, as the kernel sees it.
    colM, colW = (iM - n) % num, (iW - n) % num
    holes = {colW} | ({colM} if colM < nd else set())
    bad = {colM - nd} if colM >= nd else set()
    good = 0
    for p in range(num - nd):
        idx = (nd + p + n) % num if rotated else nd + p
        if idx != iM and p not in bad:
            good += 1
    return 'PROVEN' if len(holes) <= good else 'AMBIGUOUS'
aligned, rows = 1, []
for f0, f1, p0 in ext:
    if not (c['start'] <= p0 and p0 + (f1 - f0) <= c['start'] + c['len']):
        aligned = 0
        continue
    first = (p0 - c['start']) // fsl
    last = (p0 + (f1 - f0) - 1 - c['start']) // fsl
    for n in range(first, last + 1):
        fss = c['start'] + n * fsl
        if fss < p0 or fss + fsl > p0 + (f1 - f0):
            aligned = 0
            continue
        colW = (iW - n) % num
        if colW >= nd:
            continue
        rows.append((n, f0 + fss - p0 + colW * SL,
                     c['stripes'][iW][1] + n * SL,
                     verdict(n, True), verdict(n, False)))
print(f'M={dmidx(iM)} W={dmidx(iW)} ND={nd} NUM={num} ALIGNED={aligned}')
for r in rows:
    print('ROW %d %d %d %s %s' % r)
PY
	head -1 $LAY | grep -q '^M=' || { log "LAYOUT_FAIL $(head -3 $LAY | tr '\n' ' ')"; finish; }
	eval "$(head -1 $LAY)"
	log "layout: $(head -1 $LAY), $(grep -c '^ROW ' $LAY) rows with W holding data"
	[ "$ALIGNED" = 1 ] || { log "LAYOUT_UNALIGNED"; finish; }
	umount $MNT || { log "UMOUNT_FAIL"; finish; }
	# Pull M: remove its dm node and let btrfs forget it.
	dmsetup remove d$M || log "DM_REMOVE_FAIL d$M"
	LIVE=$(echo $DMDEVS | tr ' ' '\n' | grep -v "/d$M\$" | tr '\n' ' ')
	btrfs device scan --forget >/dev/null 2>&1
	btrfs device scan $LIVE >/dev/null 2>&1
	do_mount $OPTS,degraded $(echo $LIVE | awk '{print $1}')
	log "mounted degraded without d$M: $(grep " $MNT " /proc/mounts | awk '{print $4}')"
	# W fails every write past its first MiB.  The write-intent log (at
	# 512 KiB) and the primary superblock stay writable, so what is under
	# test is the scrub and not whether the log survives two bad devices.
	wd=$(echo $DEVS | awk -v i=$((W + 1)) '{print $i}')
	wsz=$(blockdev --getsz $wd)
	printf '0 2048 linear %s 0\n2048 %d flakey %s 2048 0 1000 1 error_writes\n' \
		$wd $((wsz - 2048)) $wd |
		{ dmsetup suspend --nolockfs --noflush d$W && dmsetup reload d$W &&
		  dmsetup resume d$W; } || log "DM_RELOAD_FAIL d$W"
	log "d$W fails writes past 1 MiB"
	# O_DIRECT, one block at a time: each write is its own read-modify-
	# write and is done by the time dd returns, so nothing is left for a
	# transaction commit to push at W before it is healed.
	dd if=/dev/zero bs=4096 count=1 status=none | tr '\000' 'B' > /tmp/bblock.prot
	grep '^ROW ' $LAY > /tmp/prot.rows
	rm -f $T/umltest/prot.acked.$TAG; touch $T/umltest/prot.acked.$TAG
	while read -r _ n fo phys vrot vunrot; do
		dd if=/tmp/bblock.prot of=$MNT/nocow bs=4096 seek=$((fo / 4096)) count=1 \
		   conv=notrunc oflag=direct status=none 2>/dev/null &&
			echo $n >> $T/umltest/prot.acked.$TAG
	done < /tmp/prot.rows
	dm_heal $W; log "healed d$W"
	sync
	log "overwrites acknowledged: $(wc -l < $T/umltest/prot.acked.$TAG) of $(wc -l < /tmp/prot.rows)"
	H=$(ls -d /sys/fs/btrfs/*-*-* 2>/dev/null | head -1)
	wv() { awk -v k=$1 '$1 == k {print $2}' $H/raid56_write_intent 2>/dev/null; }
	hv() { awk -v k=$1 '$1 == k {print $2}' $H/raid56_health 2>/dev/null; }
	skip0=$(wv scrub_skipped_stale)
	log "before scrub: stale_marks=$(hv stale_marks) recorded_blocks=$(hv recorded_blocks) scrub_skipped_stale=$skip0"
	btrfs scrub start -B $MNT > /tmp/prot.scrub 2>&1
	log "scrub rc=$?: $(grep -E 'Error summary|ERROR' /tmp/prot.scrub | tr '\n' ' ' | cut -c1-240)"
	skip1=$(wv scrub_skipped_stale)
	skipped=$(( ${skip1:-0} - ${skip0:-0} ))
	log "after scrub: stale_marks=$(hv stale_marks) recorded_blocks=$(hv recorded_blocks) scrub_skipped_stale=$skip1"
	kmsg "left untouched|rebuilding [0-9]+ sector|unrepaired sectors" 8
	# Through the filesystem, every acknowledged block must read back as
	# 'B' in either arm: where the scrub left the stale block, the read
	# still consults the record and rebuilds it.
	echo 3 > /proc/sys/vm/drop_caches
	blk() {	# dd input args -> B, A, E (read failed) or X (anything else)
		rm -f /tmp/prot.blk
		dd "$@" of=/tmp/prot.blk bs=4096 count=1 status=none 2>/dev/null &&
			[ "$(stat -c %s /tmp/prot.blk)" = 4096 ] || { echo E; return; }
		if [ "$(tr -cd 'B' < /tmp/prot.blk | wc -c)" = 4096 ]; then echo B
		elif [ "$(tr -cd 'A' < /tmp/prot.blk | wc -c)" = 4096 ]; then echo A
		else echo X; fi
	}
	while read -r _ n fo phys vrot vunrot; do
		grep -qx $n $T/umltest/prot.acked.$TAG || continue
		echo "$n $(blk if=$MNT/nocow skip=$((fo / 4096)))"
	done < /tmp/prot.rows > /tmp/prot.read
	umount $MNT || log "UMOUNT_FAIL"
	# And on W's platter: repaired ('B') or still stale ('A').
	rows=0; acked=0; acked_aff=0; acked_unaff=0; plat_aff=0; plat_unaff=0; rd_bad=0
	while read -r _ n fo phys vrot vunrot; do
		rows=$((rows+1))
		grep -qx $n $T/umltest/prot.acked.$TAG || { log "PROT row $n refused"; continue; }
		acked=$((acked+1))
		rd=$(awk -v n=$n '$1 == n {print $2}' /tmp/prot.read)
		pl=$(blk if=/dev/mapper/d$W iflag=direct skip=$((phys / 4096)))
		[ "$rd" = B ] || rd_bad=$((rd_bad+1))
		if [ "$vrot" != "$vunrot" ]; then
			acked_aff=$((acked_aff+1))
			[ "$pl" = B ] && plat_aff=$((plat_aff+1))
		else
			acked_unaff=$((acked_unaff+1))
			[ "$pl" = B ] && plat_unaff=$((plat_unaff+1))
		fi
		log "PROT row $n rot=$((n % 5)) rotated=$vrot unrotated=$vunrot read=$rd platter=$pl"
	done < /tmp/prot.rows
	log "PROT rows=$rows acked=$acked affected=$acked_aff repaired=$plat_aff unaffected=$acked_unaff repaired=$plat_unaff read_bad=$rd_bad skipped=$skipped"
	echo "$rows $acked $acked_aff $plat_aff $acked_unaff $plat_unaff $rd_bad $skipped" > $T/umltest/prot.$TAG
	dmsetup remove_all 2>/dev/null
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
