#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does a replace of a missing RAID5 device keep its record of the zeros it puts
# on the new device, where it can neither copy nor rebuild, when the
# write-intent log is full while the device is missing -- or, if a record has
# to go, does the replace fail rather than let the new device serve zeros?
#
#   replace_marks_kept.sh <kernel> [keep|lost|stale|resume|resume-lost|both|all]
#
# See replace_marks_kept in init-final3.sh.  RAID5 data, RAID1C3 metadata over
# four devices, device 3 (devid 4) pulled, the array mounted degraded, then
# 'btrfs replace start 4' onto a fifth device:
#   keep  the log is full of the degraded writes' records; two sectors the
#         replace cannot rebuild, in two regions
#     fixed    the replace finishes and both read EIO
#     control  raid56_wf_evict_replace_marks=1: the second record spends the
#              first, and its sector reads back as zeros without an error
#   lost  nothing on device 3 can be rebuilt: more records of zeros than the
#         log holds
#     fixed    the replace fails with the replace_record_dropped alert, the
#              device stays missing, no sector reads back as zeros
#     control  the replace finishes, and the sectors whose records were
#              spent read back as zeros without an error
#   stale  the two rows of keep, written into first while degraded -- their
#          column already named stale when the replace records the zeros --
#          then rows after them until the log is exactly full; the last rows
#          on device 3 cannot be rebuilt either, and each record the replace
#          makes for them spends another
#     fixed    the replace finishes and both read EIO
#     control  raid56_wf_replace_keeps_added_only=1: the two records are not
#              the replace's, it did not add the marks; they are spent first,
#              and their sectors read back as zeros
#   resume  stale, but the machine crashes once the replace has recorded
#          the two and moved past their device extent, and the last rows
#          fail only in the next boot, which mounts degraded: the log
#          reloads the two records as ordinary ones, and the replace resumes
#     fixed    it copies again from the start, recording both again as its
#              own; it finishes and both read EIO
#     control  raid56_wf_replace_keeps_added_only=1: it resumes past them;
#              the last rows' records spend theirs, and their sectors read
#              back as zeros
#   resume-lost  lost, but the machine crashes as soon as the replace runs,
#          and device 0 fails its reads only in the next boot: the replace
#          resumes at mount, in the kernel's thread, and fails
#     fixed    it fails with the replace_record_dropped alert and a message,
#              the device stays missing, nothing reads back as zeros
#     control  raid56_wf_resume_fail_warns=1: the same, with a kernel warning
#              and backtrace on top (KERNEL_SPLAT), as for a bug
#   both is keep and lost; all adds stale, resume and resume-lost.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: replace_marks_kept.sh <kernel> [keep|lost|stale|resume|resume-lost|both|all]}
WHICH=${2:-both}
NDEV=5
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-rm.sh.$$ &&
	mv -f $T/umltest/init-rm.sh.$$ $T/umltest/init-rm.sh
for f in raid56_rows.py replace_marks_rows.py; do
	cp $HERE/$f $T/umltest/$f.$$ && mv -f $T/umltest/$f.$$ $T/umltest/$f
done
ulimit -c 0
arm() {	# arm name control
	local tag=rm-$2 ubds="" phases=1 ph
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/rm.$tag $T/umltest/rm.*.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do
		truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"
	done
	case $1 in resume|resume-lost) phases="1 2";; esac
	for ph in $phases; do
		# keep, stale, resume: enough rows with data on device 3 to
		# fill the log (82).
		timeout 2400 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-rm.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=replace_marks_kept OPTS=rw TAG=$tag NDEV=4 \
			ARM=$1 CONTROL=$3 PHASE=$ph \
			RM_REGIONS=$(case $1 in *lost) echo 100;; keep) echo 140;; *) echo 150;; esac) \
			< /dev/null > $D/log.$ph 2>&1
		echo "boot $ph rc=$?" >> $D/log.$ph
	done
	cat $D/log.* > $D/log
	rm -f $D/disk*.img
}
ARMS=""
case $WHICH in
keep|lost|stale|resume|resume-lost) ARMS=$WHICH;;
both) ARMS="keep lost";;
all) ARMS="keep lost stale resume resume-lost";;
*) echo "unknown arm $WHICH"; exit 2;;
esac
for a in $ARMS; do
	arm $a $a-fixed 0
	arm $a $a-control 1
done
SHOW="control:|RM |replace rc|replace status|unreadable|fails reads|replace FAILED"
SHOW="$SHOW|dropping the record of the zeros|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL"
SHOW="$SHOW|LAYOUT_FAIL|DM_RELOAD|DM_REMOVE|KNOB_FAIL|THROTTLE_FAIL|RM_TIMEOUT"
SHOW="$SHOW|RM_NOT_RUNNING|RM_CRASH|RM layout|continuing dev_replace|copying again"
SHOW="$SHOW|resumed at mount failed|WARNING:"
res() { cat $T/umltest/rm.rm-$1 2>/dev/null || echo "? ? ? ? ? ? ? ? ?"; }
fail=0; inconclusive=0
for a in $ARMS; do
	for x in fixed control; do
		grep -ahE "$SHOW" $T/umltest/rm-$a-$x/log | grep -v "RM_ZERO\|RM_OTHER" |
			head -30 | sed "s/^/  [$a-$x] /"
	done
	read -r f_rrc f_mem f_miss f_ok f_eio f_zero f_other f_drop f_unc <<<"$(res $a-fixed)"
	read -r c_rrc c_mem c_miss c_ok c_eio c_zero c_other c_drop c_unc <<<"$(res $a-control)"
	echo "  $a: replace rc, target a member, a device missing, rows ok/eio/zero/other," \
	     "replace_record_dropped, replace_uncopyable:"
	echo "    fixed   $f_rrc $f_mem $f_miss $f_ok/$f_eio/$f_zero/$f_other $f_drop $f_unc"
	echo "    control $c_rrc $c_mem $c_miss $c_ok/$c_eio/$c_zero/$c_other $c_drop $c_unc"
	case "$f_rrc$c_rrc" in *'?'*)
		echo "  $a: INCONCLUSIVE -- a boot did not report"; inconclusive=1; continue;;
	esac
	# resume-lost: the control is to splat.
	if grep -lq KERNEL_SPLAT $T/umltest/rm-$a-fixed/log ||
	   { [ $a != resume-lost ] && grep -lq KERNEL_SPLAT $T/umltest/rm-$a-control/log; }; then
		echo "  $a: FAIL -- kernel splat"; fail=1; continue
	fi
	if grep -lq "CONTROL_KNOB_FAIL\|LAYOUT_FAIL" $T/umltest/rm-$a-*/log; then
		echo "  $a: INCONCLUSIVE -- a knob is missing or the layout did not work out"
		inconclusive=1; continue
	fi
	if [ "${f_unc:-0}" = 0 ] || [ "${c_unc:-0}" = 0 ]; then
		echo "  $a: INCONCLUSIVE -- the replace met no sector it could not copy"
		inconclusive=1; continue
	fi
	if [ "$f_zero" != 0 ] || [ "$f_other" != 0 ]; then
		echo "  $a: FAIL -- $f_zero sector(s) read back as the target's zeros," \
		     "$f_other as something else"
		fail=1; continue
	fi
	if [ $a = resume ]; then
		if ! grep -q "RM_CRASH" $T/umltest/rm-$a-fixed/log.1 ||
		   ! grep -q "RM_CRASH" $T/umltest/rm-$a-control/log.1 ||
		   ! grep -q "replace_uncopyable 2, running yes" $T/umltest/rm-$a-control/log.1
		then
			echo "  $a: INCONCLUSIVE -- a first boot did not crash with the replace running" \
			     "past both rows"
			inconclusive=1; continue
		fi
		if ! grep -q "copying again from the start" $T/umltest/rm-$a-fixed/log; then
			echo "  $a: FAIL -- the resumed replace did not copy again from the start"
			fail=1; continue
		fi
		if grep -q "copying again from the start" $T/umltest/rm-$a-control/log; then
			echo "  $a: INCONCLUSIVE -- the control's replace copied again from the start"
			inconclusive=1; continue
		fi
	fi
	if [ $a = resume-lost ]; then
		if ! grep -q "RM_CRASH" $T/umltest/rm-$a-fixed/log.1 ||
		   ! grep -q "RM_CRASH" $T/umltest/rm-$a-control/log.1; then
			echo "  $a: INCONCLUSIVE -- a first boot did not crash with the replace running"
			inconclusive=1; continue
		fi
		if [ "$f_rrc" = 0 ] || [ "$f_mem" != 0 ] || [ "$f_miss" != 1 ] ||
		   [ "${f_drop:-0}" = 0 ] || [ "$f_zero" != 0 ] ||
		   ! grep -q "resumed at mount failed" $T/umltest/rm-$a-fixed/log; then
			echo "  $a: FAIL -- the resumed replace was not failed with the alert and" \
			     "a message (rc $f_rrc, member $f_mem, missing $f_miss," \
			     "replace_record_dropped $f_drop, zero $f_zero)"
			fail=1; continue
		fi
		# That warning, not a splat of any kind.
		if [ "$c_rrc" = 0 ] || ! grep -q "WARNING: .*btrfs_dev_replace_kthread" \
			$T/umltest/rm-$a-control/log; then
			echo "  $a: INCONCLUSIVE -- the control's resumed replace did not fail with a" \
			     "kernel warning from btrfs_dev_replace_kthread (rc $c_rrc)"
			inconclusive=1; continue
		fi
		echo "  $a: PASS -- a replace resumed at mount that lost a record of zeros failed with"
		echo "        the alert and a message, no warning; the control warned with a backtrace"
		continue
	fi
	if [ $a != lost ]; then
		if [ "$f_rrc" != 0 ] || [ "$f_mem" != 1 ] || [ "$f_eio" != 2 ]; then
			echo "  $a: FAIL -- the replace did not finish" \
			     "(rc $f_rrc, member $f_mem), or the two sectors did not read EIO ($f_eio)"
			fail=1; continue
		fi
		if [ "$c_zero" = 0 ]; then
			echo "  $a: INCONCLUSIVE -- the control read no zeros either"
			inconclusive=1; continue
		fi
		case $a in
		keep)	echo "  $a: PASS -- both records of zeros survived a full degraded log, both"
			echo "        sectors read EIO; spent in table order, $c_zero read back as zeros";;
		stale)	echo "  $a: PASS -- both records of zeros under a column named stale before"
			echo "        survived a full degraded log, both sectors read EIO; not the"
			echo "        replace's, $c_zero read back as zeros";;
		resume)	echo "  $a: PASS -- the replace resumed after a crash recorded both again as its"
			echo "        own, both sectors read EIO; resumed past them, $c_zero read back as zeros";;
		esac
	else
		if [ "$f_rrc" = 0 ] || [ "$f_mem" != 0 ] || [ "$f_miss" != 1 ] ||
		   [ "${f_drop:-0}" = 0 ]; then
			echo "  $a: FAIL -- the replace was not failed with the alert (rc $f_rrc," \
			     "member $f_mem, missing $f_miss, replace_record_dropped $f_drop)"
			fail=1; continue
		fi
		if [ "$c_rrc" != 0 ] || [ "$c_zero" = 0 ]; then
			echo "  $a: INCONCLUSIVE -- the control's replace did not finish" \
			     "with zeros read back (rc $c_rrc, zero $c_zero)"
			inconclusive=1; continue
		fi
		echo "  $a: PASS -- a replace that lost a record of zeros failed with the alert"
		echo "        and left the device missing ($f_eio EIO, 0 zeros); spent in table"
		echo "        order, the replace finished and $c_zero sectors read back as zeros"
	fi
done
[ $fail = 1 ] && { echo "RESULT: FAIL"; exit 1; }
[ $inconclusive = 1 ] && { echo "RESULT: INCONCLUSIVE"; exit 2; }
echo "RESULT: PASS -- a replace's records of zeros on its target are kept in a full"
echo "        degraded log, and a replace that loses one fails instead of serving them"
