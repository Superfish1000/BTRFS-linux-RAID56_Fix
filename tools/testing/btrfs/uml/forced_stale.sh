#!/bin/bash
# Does scrub take back a block the write-intent log names stale when the
# device holding it reads fine and the rebuild that should replace it cannot
# run?
#
#   forced_stale.sh <kernel>
#
# See forced_stale in init-final3.sh.  RAID5 data over four devices; W fails
# writes while one block of its nodatacow data column is overwritten in each
# full stripe whose parity is on V, and is healed.  The first scrub runs with
# V failing reads, so the parity rebuild of W's stale block cannot happen and
# only the scrub's last-resort re-read of W itself is left.
#   fixed    the re-read's bytes stay in error: the full stripe is reported
#            unrepaired, V's parity still holds the acknowledged 'B', the
#            record is kept, a read returns 'B', and a second scrub with V
#            healthy writes 'B' onto W's platter
#   control  raid56_scrub_reread_trusts_stale=1: the stale 'A' is accepted,
#            the parity regenerated from it and the record retired -- the
#            acknowledged block is gone and reads back as 'A' with no error
#
# Then the same first scrub again, and V, still failing reads, replaced with a
# fifth device (FST_REPLACE=1).  The replace cannot rebuild W's column either
# and declines to regenerate the parity, and cannot copy V's parity:
#   replace-fixed    it writes zeros there and records that parity stale; the
#                    replace finishes and counts them, a read of W's block and
#                    of the untouched 'A' next to it fails with EIO, and the
#                    next scrub leaves W's column alone and keeps the record
#   replace-control  raid56_wf_replace_skips_refused_parity=1: the target's
#                    parity is left as it was (zeros) and counts as good; both
#                    reads return zeros without an error, and the next scrub
#                    writes zeros over W's column and retires the record
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: forced_stale.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-fst.sh.$$ && mv -f $T/umltest/init-fst.sh.$$ $T/umltest/init-fst.sh	# atomic: a guest may be reading it
ulimit -c 0
arm() {	# name control replace
	local tag=fst-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/fst.$tag $T/umltest/fsr.$tag $T/umltest/fst.layout.$tag $T/umltest/fst.acked.$tag $T/umltest/results.$tag
	# The replace target is one device more than the array.
	for i in $(seq 0 $((NDEV + $3 - 1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-fst.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=forced_stale OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 FST_REPLACE=$3 FST_ROWS=${FST_ROWS:-20} < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm fixed 0 0
arm control 1 0
arm replace-fixed 0 1
arm replace-control 1 1
for a in fixed control replace-fixed replace-control; do
	grep -ah "layout:\|acknowledged\|scrub [12] rc\|before scrub\|after scrub\|after the replace\|replace rc\|replace status\|FST \|FSR \|control:\|re-read from mirror\|unrepaired sectors\|neither regenerate nor copy\|left untouched\|KNOB_FAIL\|LAYOUT_\|DM_RELOAD\|NEED_\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL" \
		$T/umltest/fst-$a/log | sed "s/^/  [$a] /"
done
grep -lq KERNEL_SPLAT $T/umltest/fst-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
# Each verdict prints its reason and returns 0 PASS, 1 FAIL or 2 INCONCLUSIVE.
reread_verdict() {
	# rows acked stale_after1 parity_kept parity_lost read_ok repaired_after2 rec0 rec1 rec2 refusals
	local f_rows f_acked f_stale f_kept f_lost f_read f_rep f_rec0 f_rec1 f_rec2 f_ref
	local c_rows c_acked c_stale c_kept c_lost c_read c_rep c_rec0 c_rec1 c_rec2 c_ref
	read -r f_rows f_acked f_stale f_kept f_lost f_read f_rep f_rec0 f_rec1 f_rec2 f_ref < $T/umltest/fst.fst-fixed 2>/dev/null || f_rows=?
	read -r c_rows c_acked c_stale c_kept c_lost c_read c_rep c_rec0 c_rec1 c_rec2 c_ref < $T/umltest/fst.fst-control 2>/dev/null || c_rows=?
	case "$f_rows$c_rows" in *'?'*) echo "re-read: INCONCLUSIVE -- a boot did not report (see LAYOUT_ lines)"; return 2;; esac
	if [ "$f_acked" = 0 ] || [ "$c_acked" = 0 ] || [ "$f_rec0" = 0 ] || [ "$c_rec0" = 0 ]; then
		echo "re-read: INCONCLUSIVE -- no acknowledged overwrite left a record (acked $f_acked/$c_acked, recorded $f_rec0/$c_rec0)"; return 2
	fi
	# Both arms have to have reached the re-read: W still stale after the
	# first scrub means the rebuild really could not run.  If it did, the
	# parity was readable and neither arm says anything about the re-read.
	if [ "$f_stale" != "$f_acked" ] || [ "$c_stale" != "$c_acked" ]; then
		echo "re-read: INCONCLUSIVE -- the first scrub rebuilt blocks it should not have been able to (W stale after it: fixed $f_stale of $f_acked, control $c_stale of $c_acked)"; return 2
	fi
	# The control's damage shows either way: a parity regenerated from the
	# stale data, or -- with V still failing reads, so no parity can be
	# written -- the record retired on the strength of the re-read, after
	# which reads return the stale content with no error.
	if [ "$c_lost" = 0 ] && [ "$c_read" = "$c_acked" ] && [ "$c_rec1" != 0 ]; then
		echo "re-read: INCONCLUSIVE -- the control neither regenerated a parity from the stale blocks nor retired their record, so a clean fixed arm proves nothing"; return 2
	fi
	if [ "$f_lost" != 0 ] || [ "$f_kept" != "$f_acked" ]; then
		echo "re-read: FAIL -- the first scrub replaced the parity of $f_lost of $f_acked acknowledged blocks (kept $f_kept)"; return 1
	fi
	if [ "$f_rec1" = 0 ]; then
		echo "re-read: FAIL -- the parity survived but the record was retired, so nothing will ever rebuild the block"; return 1
	fi
	if [ "$f_ref" = 0 ]; then
		echo "re-read: FAIL -- the stale blocks were kept in error without a word in the kernel log"; return 1
	fi
	if [ "$f_read" != "$f_acked" ]; then
		echo "re-read: FAIL -- only $f_read of $f_acked acknowledged blocks read back as written after the first scrub"; return 1
	fi
	if [ "$f_rep" != "$f_acked" ]; then
		echo "re-read: FAIL -- with every device healthy the second scrub put back only $f_rep of $f_acked blocks"; return 1
	fi
	echo "re-read: PASS -- with the parity unreadable the scrub kept all $f_acked stale blocks in error"
	echo "        and the record, a read returned what was written, and the next scrub repaired"
	echo "        them; trusting the re-read, it regenerated $c_lost parity block(s) from the stale"
	echo "        data, left $c_rec1 of $c_rec0 record block(s), and $((c_acked - c_read)) of $c_acked acknowledged"
	echo "        block(s) no longer read back as written"
	return 0
}
replace_verdict() {
	# rows acked stale_after1 parity_kept rec0 rec1 replace_rc uncorr uncopyable lost_msgs
	# ok eio wrong changed rec2 -- reads of the block and its neighbour after
	# the replace (ok, eio, wrong) and blocks of W's column scrub 2 changed
	local f_rows f_acked f_stale f_kept f_rec0 f_rec1 f_rrc f_unc f_unco f_msg f_ok f_eio f_wrong f_chg f_rec2
	local c_rows c_acked c_stale c_kept c_rec0 c_rec1 c_rrc c_unc c_unco c_msg c_ok c_eio c_wrong c_chg c_rec2
	read -r f_rows f_acked f_stale f_kept f_rec0 f_rec1 f_rrc f_unc f_unco f_msg f_ok f_eio f_wrong f_chg f_rec2 \
		< $T/umltest/fsr.fst-replace-fixed 2>/dev/null || f_rows=?
	read -r c_rows c_acked c_stale c_kept c_rec0 c_rec1 c_rrc c_unc c_unco c_msg c_ok c_eio c_wrong c_chg c_rec2 \
		< $T/umltest/fsr.fst-replace-control 2>/dev/null || c_rows=?
	case "$f_rows$c_rows" in *'?'*) echo "replace: INCONCLUSIVE -- a boot did not report (see LAYOUT_ and NEED_ lines)"; return 2;; esac
	if [ "$f_acked" = 0 ] || [ "$c_acked" = 0 ] || [ "$f_rec0" = 0 ] || [ "$c_rec0" = 0 ]; then
		echo "replace: INCONCLUSIVE -- no acknowledged overwrite left a record (acked $f_acked/$c_acked, recorded $f_rec0/$c_rec0)"; return 2
	fi
	# What the replace has to deal with is what the fixed re-read arm
	# leaves: W stale, the acknowledged 'B' only in V's parity, the record
	# kept.  Anything else is that arm's failure, not this one's.
	if [ "$f_stale" != "$f_acked" ] || [ "$c_stale" != "$c_acked" ] ||
	   [ "$f_kept" != "$f_acked" ] || [ "$c_kept" != "$c_acked" ] ||
	   [ "$f_rec1" = 0 ] || [ "$c_rec1" = 0 ]; then
		echo "replace: INCONCLUSIVE -- the first scrub did not leave W stale, 'B' in V's parity and the record (fixed $f_stale/$f_kept, control $c_stale/$c_kept of $f_acked/$c_acked, recorded $f_rec1/$c_rec1)"; return 2
	fi
	if [ "$c_rrc" != 0 ]; then
		echo "replace: INCONCLUSIVE -- the control's replace did not finish (rc $c_rrc)"; return 2
	fi
	if [ "$((c_wrong + c_chg))" = 0 ]; then
		echo "replace: INCONCLUSIVE -- with the target's parity left as it was, nothing read or scrubbed back wrong, so a clean fixed arm proves nothing"; return 2
	fi
	if [ "$f_rrc" != 0 ]; then
		echo "replace: FAIL -- the replace did not finish (rc $f_rrc)"; return 1
	fi
	if [ "$f_wrong" != 0 ]; then
		echo "replace: FAIL -- $f_wrong read(s) after the replace returned data nothing wrote there"; return 1
	fi
	if [ "$f_chg" != 0 ]; then
		echo "replace: FAIL -- the scrub after the replace wrote over $f_chg block(s) of W's column"; return 1
	fi
	if [ "$f_rec2" = 0 ]; then
		echo "replace: FAIL -- the record was retired, so the stripe no longer says what it lost"; return 1
	fi
	if [ "${f_unc:-0}" -lt 1 ] 2>/dev/null || [ "${f_unco:-0}" -lt 1 ] 2>/dev/null || [ "$f_msg" = 0 ]; then
		echo "replace: FAIL -- the parity the replace could not copy went uncounted or unsaid (uncorrectable $f_unc, replace_uncopyable $f_unco, messages $f_msg)"; return 1
	fi
	echo "replace: PASS -- replacing V, which could not return the parity, counted $f_unc sector(s) and"
	echo "        recorded that parity stale: $f_eio read(s) failed with EIO rather than return anything, and"
	echo "        the next scrub left W's column alone; leaving the target's parity as it was, $c_wrong read(s)"
	echo "        returned what nothing wrote and the next scrub wrote over $c_chg block(s) of W's column"
	return 0
}
reread_verdict; r1=$?
replace_verdict; r2=$?
if [ $r1 = 1 ] || [ $r2 = 1 ]; then echo "RESULT: FAIL"; exit 1; fi
if [ $r1 = 2 ] || [ $r2 = 2 ]; then echo "RESULT: INCONCLUSIVE"; exit 2; fi
echo "RESULT: PASS"
