#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does a write into a full stripe that a RAID5 device replace has already
# copied give the new device the old device's sectors its parity was computed
# from?
#
#   replace_rmw_resident.sh <kernel>
#
# See replace_rmw_resident in init-final3.sh.  RAID5 over three devices; one
# full stripe holds random bytes, as a reused disk does, a preallocated file in
# column 0 and no extent in column 1.  The replace of column 1's device copies
# that chunk (column 1 rebuilt from the parity, which there is not what the old
# device holds), and while it goes on, throttled, through the later chunks, a
# nodatasum write goes into the preallocated file in place: a
# read-modify-write that computes its parity from the old device's column 1.  After the replace, column 0's device is left out and the
# write is read back, rebuilt from that parity and the new device.
#   fixed    the write copied the old device's sectors it read to the new one,
#            and the block reads back as written
#   control  raid56_wf_replace_skips_resident=1: only the sectors the write
#            supplied reach the new device, and the block reads back as
#            something nobody wrote, with no error
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: replace_rmw_resident.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-rrr.sh.$$ && mv -f $T/umltest/init-rrr.sh.$$ $T/umltest/init-rrr.sh	# atomic: a guest may be reading it
cp $HERE/raid56_chunks.py $T/umltest/raid56_chunks.py.$$ && mv -f $T/umltest/raid56_chunks.py.$$ $T/umltest/raid56_chunks.py
ulimit -c 0
arm() {	# name control
	local tag=rrr-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/rrr.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-rrr.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=replace_rmw_resident OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
for a in fixed control; do
	grep -ah "first data chunk\|layout:\|filler:\|replace at\|B written\|replace rc\|replace status\|without \|RRR \|control:\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|LAYOUT_FAIL\|JUNK_FAIL\|FILL_FAIL\|THROTTLE_FAIL\|B_WRITE_FAIL\|KNOB_FAIL\|preallocated at\|full stripe " \
		$T/umltest/rrr-$a/log | head -30 | sed "s/^/  [$a] /"
done
read -r f_rrc f_dur f_inp f_rb f_nb f_fok < $T/umltest/rrr.rrr-fixed 2>/dev/null || f_rrc=?
read -r c_rrc c_dur c_inp c_rb c_nb c_fok < $T/umltest/rrr.rrr-control 2>/dev/null || c_rrc=?
case "$f_rrc$c_rrc" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report (see LAYOUT_FAIL)"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/rrr-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
echo "  fixed:   replace rc $f_rrc, written during it $f_dur, in place $f_inp; b read $f_rb bytes, $f_nb 'B'; filler right $f_fok"
echo "  control: replace rc $c_rrc, written during it $c_dur, in place $c_inp; b read $c_rb bytes, $c_nb 'B'; filler right $c_fok"
for a in f c; do
	eval "rrc=\$${a}_rrc dur=\$${a}_dur inp=\$${a}_inp fok=\$${a}_fok"
	if [ "$rrc" != 0 ] || [ "$dur" != 1 ] || [ "$inp" != 1 ] || [ "$fok" != 1 ]; then
		echo "RESULT: INCONCLUSIVE -- an arm did not write in place into the copied stripe during a replace that finished, or lost a filler file"
		exit 2
	fi
done
if [ "$c_rb" != 16384 ] || [ "$c_nb" = 16384 ]; then
	echo "RESULT: INCONCLUSIVE -- the control read back $c_nb of 16384 'B' ($c_rb bytes): the stripe did not reach the state"
	exit 2
fi
if [ "$f_rb" != 16384 ]; then
	echo "RESULT: FAIL -- the fixed arm could not read the block back ($f_rb bytes)"; exit 1
fi
if [ "$f_nb" != 16384 ]; then
	echo "RESULT: FAIL -- the block read back as something nobody wrote ($f_nb of 16384 'B'), without an error"; exit 1
fi
echo "RESULT: PASS -- the write during the replace kept the new device consistent with its parity:"
echo "        the block reads back as written with its column's device gone (control: $c_nb of 16384 right, no error)"
