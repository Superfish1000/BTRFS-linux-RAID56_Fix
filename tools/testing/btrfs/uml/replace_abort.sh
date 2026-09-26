#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does a RAID5 device replace that is aborted because it cannot record what it
# can neither copy nor rebuild keep raid56_health failing until someone has
# seen it?
#
#   replace_abort.sh <kernel>
#
# See replace_uncopyable in init-final3.sh, here with RUA=1 and mounted with
# -o noraid56_write_intent: the write-intent log is not enabled, so the replace
# cannot record the sector it can neither copy nor rebuild, and it is aborted
# rather than leave zeros on the new device.  Nothing is recorded, so nothing
# else holds the state failing.
#   fixed    the replace fails; raid56_health reads failing, unacknowledged
#            replace_aborted, until 'echo ack', then ok
#   control  raid56_wf_replace_abort_unlatched=1: the replace fails, and as
#            soon as the alert work has run raid56_health reads ok, nothing
#            unacknowledged -- a monitor polling it, or reading the uevent
#            (BTRFS_RAID56_HEALTH=ok), never sees the refused replace
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: replace_abort.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-rab.sh.$$ &&
	mv -f $T/umltest/init-rab.sh.$$ $T/umltest/init-rab.sh
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py.$$ &&
	mv -f $T/umltest/raid56_layout.py.$$ $T/umltest/raid56_layout.py
ulimit -c 0
arm() {	# name [kernel args...]
	local tag=rab-$1 ubds=""
	local D=$T/umltest/$tag
	shift
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/rua.$tag $T/umltest/ruc.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do
		truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"
	done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-rab.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=replace_uncopyable OPTS=rw,noraid56_write_intent \
		PROFILE=raid5:raid1 TAG=$tag NDEV=$NDEV CONTROL=0 RUA=1 "$@" \
		< /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm fixed
arm control btrfs.raid56_wf_replace_abort_unlatched=1
SHOW="replace rc|RUA |replace was ABORTED|cannot record them|health:|unreadable:"
SHOW="$SHOW|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|LAYOUT_FAIL|DM_RELOAD|ACK_FAIL"
for a in fixed control; do
	grep -ahE "$SHOW" $T/umltest/rab-$a/log | cut -c1-300 | head -20 | sed "s/^/  [$a] /"
done
# replace rc, state, unacknowledged, state and unacknowledged after the ack,
# replace_aborted count
res() { cat $T/umltest/rua.rab-$1 2>/dev/null || echo "? ? ? ? ? ?"; }
read -r f_rrc f_st f_ua f_st2 f_ua2 f_cnt <<<"$(res fixed)"
read -r c_rrc c_st c_ua c_st2 c_ua2 c_cnt <<<"$(res control)"
echo "  replace rc, state, unacknowledged / after the ack, replace_aborted:"
echo "    fixed   $f_rrc $f_st $f_ua / $f_st2 $f_ua2, $f_cnt"
echo "    control $c_rrc $c_st $c_ua / $c_st2 $c_ua2, $c_cnt"
case "$f_rrc$c_rrc" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
if grep -lq KERNEL_SPLAT $T/umltest/rab-*/log; then
	echo "RESULT: FAIL -- kernel splat"
	exit 1
fi
for a in fixed control; do
	grep -aq "cannot record them because the write-intent log is not enabled" \
		$T/umltest/rab-$a/log && continue
	echo "RESULT: INCONCLUSIVE -- $a: the replace was not aborted for want of a record"
	exit 2
done
if [ "$c_rrc" = 0 ] || [ "$c_st" != ok ]; then
	echo "RESULT: INCONCLUSIVE -- the control's replace did not fail, or something else" \
	     "held the state (rc $c_rrc, state $c_st)"
	exit 2
fi
if [ "$f_rrc" = 0 ]; then
	echo "RESULT: FAIL -- the replace finished without a record of what it could not copy"
	exit 1
fi
case ",$f_ua," in
*,replace_aborted,*) ;;
*)	echo "RESULT: FAIL -- the aborted replace is not held unacknowledged ($f_ua)"; exit 1;;
esac
if [ "$f_st" != failing ] || [ "${f_cnt:-0}" -lt 1 ]; then
	echo "RESULT: FAIL -- raid56_health reads $f_st after the aborted replace" \
	     "(replace_aborted $f_cnt)"
	exit 1
fi
if [ "$f_st2" != ok ] || [ "$f_ua2" != none ]; then
	echo "RESULT: FAIL -- 'echo ack' did not end it (state $f_st2," \
	     "unacknowledged $f_ua2)"
	exit 1
fi
echo "RESULT: PASS -- the replace aborted for want of a record reads failing, replace_aborted"
echo "        unacknowledged, until acknowledged; without the latch it read $c_st at once"
