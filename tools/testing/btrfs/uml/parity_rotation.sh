#!/bin/bash
# Does scrub look for a full stripe's parity where the rotation put it?
#
#   parity_rotation.sh <kernel>
#
# See parity_rotation in init-final3.sh.  RAID6 data over five devices, the
# device at chunk stripe 3 removed and the array mounted degraded; a second
# device fails writes while one block of its data column is overwritten in
# every full stripe where it holds one, and is then healed and scrubbed.  Each
# of those full stripes has at most two columns nothing vouches for, and the
# parity to rebuild them.
#   fixed    every acknowledged block is rebuilt onto the healed device's
#            platter, and scrub declines no full stripe
#   control  raid56_scrub_parity_unrotated=1 counts P and Q on chunk stripes 3
#            and 4 whatever the full stripe: where that is the missing
#            device's data column or its Q, scrub declares the stripe
#            ambiguous and leaves the stale block.  The rotation-0 full
#            stripes, where both lookups agree, are repaired in both arms.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: parity_rotation.sh <kernel>}
NDEV=5
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-prot.sh.$$ && mv -f $T/umltest/init-prot.sh.$$ $T/umltest/init-prot.sh	# atomic: a guest may be reading it
ulimit -c 0
arm() {	# name control
	local tag=prot-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/prot.$tag $T/umltest/prot.layout.$tag $T/umltest/prot.acked.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-prot.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=parity_rotation OPTS=rw PROFILE=raid6:raid1c3 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 PR_ROWS=${PR_ROWS:-20} < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
for a in fixed control; do
	grep -ah "layout:\|mounted degraded\|acknowledged\|scrub rc\|before scrub\|after scrub\|PROT \|control:\|KNOB_FAIL\|LAYOUT_\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL" \
		$T/umltest/prot-$a/log | sed "s/^/  [$a] /"
done
# rows acked affected affected_repaired unaffected unaffected_repaired read_bad skipped
read -r f_rows f_acked f_aff f_paff f_unaff f_punaff f_rdbad f_skip < $T/umltest/prot.prot-fixed 2>/dev/null || f_rows=?
read -r c_rows c_acked c_aff c_paff c_unaff c_punaff c_rdbad c_skip < $T/umltest/prot.prot-control 2>/dev/null || c_rows=?
case "$f_rows$c_rows" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report (see LAYOUT_ lines)"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/prot-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
if [ "$f_aff" = 0 ] || [ "$c_aff" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- no acknowledged overwrite landed in a full stripe the rotation decides"; exit 2
fi
# The control has to reproduce the defect, and only the defect: the rotated
# full stripes left stale, the rotation-0 ones repaired as in the fixed arm.
if [ "$c_paff" -ge "$c_aff" ] || [ "$c_skip" -le 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control repaired the rotated stripes ($c_paff of $c_aff, skipped $c_skip), so a clean fixed arm proves nothing"; exit 2
fi
if [ "$c_punaff" != "$c_unaff" ]; then
	echo "RESULT: INCONCLUSIVE -- the control did not repair even the rotation-0 stripes ($c_punaff of $c_unaff): something other than the lookup is in the way"; exit 2
fi
if [ "$f_rdbad" != 0 ] || [ "$c_rdbad" != 0 ]; then
	echo "RESULT: FAIL -- an acknowledged block did not read back: fixed $f_rdbad, control $c_rdbad"; exit 1
fi
if [ "$f_paff" != "$f_aff" ] || [ "$f_punaff" != "$f_unaff" ] || [ "$f_skip" != 0 ]; then
	echo "RESULT: FAIL -- scrub left stale blocks: rotated $f_paff of $f_aff, rotation-0 $f_punaff of $f_unaff repaired, $f_skip declined"; exit 1
fi
echo "RESULT: PASS -- all $f_acked stale blocks rebuilt onto the platter, none declined;"
echo "        without the rotation $((c_aff - c_paff)) of $c_aff were left stale (declined $c_skip times), the $c_unaff rotation-0 ones repaired"
