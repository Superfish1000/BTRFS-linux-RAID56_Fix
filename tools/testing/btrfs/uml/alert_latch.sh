#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does an alert that waits for an acknowledgment survive a crash and a
# remount, and does the acknowledgment?
#
#   alert_latch.sh <kernel>
#
# See alert_latch in init-final3.sh.  RAID5 data, RAID1C3 metadata, four
# devices.  The log slots of three of them fail writes, and a write fails
# before it reaches a disk: log_write_failed, which stays until 'ack'.  They
# are healed, another write goes through, and the machine stops without
# syncing.  Then the array is mounted, unmounted and mounted again, the alert
# acknowledged, and the machine stops again; one more mount.
#   fixed    the log carries the alert (BTRFS_WIB_LATCH_OFFSET): raid56_health
#            shows it unacknowledged after the crash and after the remount,
#            the state failing, and not after the ack and the crash after it
#   control  raid56_wf_latch_volatile=1: the alert is in memory only, and the
#            mount after the crash shows nothing unacknowledged
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: alert_latch.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-al.sh.$$ &&
	mv -f $T/umltest/init-al.sh.$$ $T/umltest/init-al.sh
ulimit -c 0
arm() {	# name control
	local tag=al-$1 control=$2
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/al.$tag.* $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 256M $D/disk$i.img; done
	boot() {	# phase opts
		local ubds="" d
		for d in $(seq 0 $((NDEV-1))); do ubds="$ubds ubd$d=$D/disk$d.img"; done
		timeout 600 $KERNEL mem=512M rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-al.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=alert_latch OPTS=$2 PROFILE=raid5:raid1c3 TAG=$tag \
			NDEV=$NDEV CONTROL=$control MNTDEV=/dev/ubda PHASE=$1 \
			< /dev/null > $D/log.$1 2>&1
		echo "boot $1 rc=$?" >> $D/log.boots
	}
	# commit=600: no transaction commit meets the failing log slots.
	boot raise rw,commit=600
	boot check rw
	boot final rw
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
for a in fixed control; do
	grep -ahE "control:|AL |alerts an earlier mount|KERNEL_SPLAT|WATCHDOG|_FAIL" \
		$T/umltest/al-$a/log.* | cut -c1-300 | sed "s/^/  [$a] /"
done
res() { cat $T/umltest/al.al-$1.$2 2>/dev/null || echo "? ? ? ?"; }
read -r f_w1 f_w2 f_u0 <<<"$(res fixed raise)"
read -r c_w1 c_w2 c_u0 <<<"$(res control raise)"
read -r f_u1 f_u2 f_u3 f_st <<<"$(res fixed check)"
read -r c_u1 c_u2 c_u3 c_st <<<"$(res control check)"
read -r f_u4 <<<"$(res fixed final)"
read -r c_u4 <<<"$(res control final)"
echo "  unacknowledged, raised / after the crash / after a remount / after the ack /" \
     "after the next crash:"
echo "    fixed   $f_u0 / $f_u1 / $f_u2 / $f_u3 / $f_u4 (state $f_st)"
echo "    control $c_u0 / $c_u1 / $c_u2 / $c_u3 / $c_u4 (state $c_st)"
case "$f_w1$c_w1$f_u1$c_u1$f_u4$c_u4" in
*'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;;
esac
grep -lq KERNEL_SPLAT $T/umltest/al-*/log.* && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq WATCHDOG $T/umltest/al-*/log.* && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/al-control/log.* &&
	{ echo "RESULT: INCONCLUSIVE -- the control knob is not there"; exit 2; }
has() { case ",$1," in *,log_write_failed,*) return 0;; esac; return 1; }
if [ "$f_w1" != eio ] || [ "$c_w1" != eio ] || ! has "$f_u0" || ! has "$c_u0"; then
	echo "RESULT: INCONCLUSIVE -- the write did not fail with log_write_failed"
	echo "        (fixed $f_w1 $f_u0, control $c_w1 $c_u0)"
	exit 2
fi
if [ "$c_u1" != none ]; then
	echo "RESULT: INCONCLUSIVE -- the control kept the alert across the crash ($c_u1)"
	exit 2
fi
if ! has "$f_u1" || ! has "$f_u2" || [ "$f_st" != failing ]; then
	echo "RESULT: FAIL -- the alert did not survive the crash ($f_u1) or the remount ($f_u2)," \
	     "state $f_st"
	exit 1
fi
if [ "$f_u3" != none ] || [ "$f_u4" != none ]; then
	echo "RESULT: FAIL -- the acknowledged alert came back ($f_u3, after the crash $f_u4)"
	exit 1
fi
grep -aq "alerts an earlier mount raised that nobody acknowledged: log_write_failed" \
	$T/umltest/al-fixed/log.check ||
	{ echo "RESULT: FAIL -- the mount did not say which alerts it took back"; exit 1; }
echo "RESULT: PASS -- log_write_failed stood after a crash and a remount until acknowledged,"
echo "        and the acknowledgment survived the next crash; control: forgotten at the crash"
