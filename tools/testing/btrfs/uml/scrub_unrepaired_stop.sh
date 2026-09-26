#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does a RAID5 scrub go on past a full stripe it cannot repair?
#
#   scrub_unrepaired_stop.sh <kernel>
#
# See scrub_unrepaired_stop in init-final3.sh.  RAID5 over three devices; a
# full stripe F whose last data column holds no extent has a checksummed
# sector its parity cannot rebuild, and the parity of a later full stripe on
# the same device as F's is wrong.
#   fixed    the scrub reports F unrepaired and goes on: the later parity is
#            regenerated
#   control  raid56_scrub_unrepaired_stops=1: the scrub of F's parity device
#            takes F for the end of the chunk, and the later parity stays
#            wrong, with nothing said about it
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: scrub_unrepaired_stop.sh <kernel>}
NDEV=3
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-sus.sh.$$ && mv -f $T/umltest/init-sus.sh.$$ $T/umltest/init-sus.sh	# atomic: a guest may be reading it
cp $HERE/raid56_chunks.py $T/umltest/raid56_chunks.py.$$ && mv -f $T/umltest/raid56_chunks.py.$$ $T/umltest/raid56_chunks.py
ulimit -c 0
arm() {	# name control
	local tag=sus-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/sus.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-sus.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=scrub_unrepaired_stop OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
for a in fixed control; do
	grep -ah "full stripe \|scrub rc\|SUS \|control:\|unrepaired sectors\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|LAYOUT_FAIL\|CORRUPT_FAIL\|KNOB_FAIL" \
		$T/umltest/sus-$a/log | head -20 | sed "s/^/  [$a] /"
done
read -r f_rc f_corr f_unc f_msg f_pz f_pZ < $T/umltest/sus.sus-fixed 2>/dev/null || f_rc=?
read -r c_rc c_corr c_unc c_msg c_pz c_pZ < $T/umltest/sus.sus-control 2>/dev/null || c_rc=?
case "$f_rc$c_rc" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report (see LAYOUT_FAIL)"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/sus-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
if [ "${f_msg:-0}" -lt 1 ] || [ "${c_msg:-0}" -lt 1 ]; then
	echo "RESULT: INCONCLUSIVE -- a scrub did not report the full stripe unrepaired (fixed $f_msg, control $c_msg)"
	exit 2
fi
if [ "$c_pZ" != 4096 ]; then
	echo "RESULT: INCONCLUSIVE -- the control's scrub regenerated the later parity ($c_pz of 4096 zero bytes), so the stop did not happen"
	exit 2
fi
if [ "$f_pz" != 4096 ]; then
	echo "RESULT: FAIL -- the scrub left the later parity wrong ($f_pz of 4096 bytes right, $f_pZ as corrupted)"; exit 1
fi
echo "RESULT: PASS -- past the unrepaired full stripe the scrub went on and regenerated the later parity"
echo "        (control: stopped there, parity left as corrupted, scrub rc $c_rc against fixed $f_rc)"
