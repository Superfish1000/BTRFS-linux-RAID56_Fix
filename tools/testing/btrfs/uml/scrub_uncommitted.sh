#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does a RAID5 scrub decide a full stripe from a commit root that does not
# show the running transaction's write into it?
#
#   scrub_uncommitted.sh <kernel>
#
# See scrub_uncommitted in init-final3.sh.  RAID5 over three devices,
# nodatasum; a file of 'B' is written into the free column of a full stripe
# whose other column holds a committed file, while its device fails the
# write: acknowledged, 'B' only in the parity, the record naming the column.
# The device is healed and a scrub runs before anything commits.
#   commit   the scrub commits first and rebuilds 'B' onto the platter
#   decline  raid56_scrub_no_commit=1: the scrub leaves the stripe untouched
#            with the scrub_uncommitted alert, 'B' reads back from the parity,
#            and a second scrub without the knob rebuilds it
#   control  raid56_scrub_ignores_uncommitted=1: the scrub regenerates the
#            parity from the stale column and retires the record, and 'B'
#            reads back as the old content, with no error
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: scrub_uncommitted.sh <kernel>}
NDEV=3
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-suu.sh.$$ && mv -f $T/umltest/init-suu.sh.$$ $T/umltest/init-suu.sh	# atomic: a guest may be reading it
cp $HERE/raid56_chunks.py $T/umltest/raid56_chunks.py.$$ && mv -f $T/umltest/raid56_chunks.py.$$ $T/umltest/raid56_chunks.py
ulimit -c 0
arm() {	# name
	local tag=suu-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/suu.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-suu.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=scrub_uncommitted OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV SUS_ARM=$1 < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
for a in commit decline control; do arm $a; done
for a in commit decline control; do
	grep -ah "full stripe \|b at \|scrub rc\|second scrub\|arm \|SUU \|left untouched\|a scrub left\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|LAYOUT_FAIL\|KNOB_FAIL" \
		$T/umltest/suu-$a/log | head -20 | cut -c1-240 | sed "s/^/  [$a] /"
done
# The value of key $2 in arm $1's report, or ?.
v() { sed -n "s/.*\<$2=\([^ ]*\).*/\1/p" $T/umltest/suu.suu-$1 2>/dev/null | head -1 | grep . || echo '?'; }
for a in commit decline control; do
	[ -s $T/umltest/suu.suu-$a ] || { echo "RESULT: INCONCLUSIVE -- the $a arm did not report (see LAYOUT_FAIL)"; exit 2; }
done
grep -lq KERNEL_SPLAT $T/umltest/suu-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
for a in commit decline control; do
	if [ "$(v $a wrc)" != 0 ] || [ "$(v $a marks)" = 0 ] || [ "$(v $a marks)" = '?' ]; then
		echo "RESULT: INCONCLUSIVE -- in the $a arm the write into the failing column was not acknowledged and recorded (write rc $(v $a wrc), stale marks $(v $a marks))"
		exit 2
	fi
	if [ "$(v $a commits_before)" != 0 ]; then
		echo "RESULT: INCONCLUSIVE -- in the $a arm a transaction committed between the write and the scrub ($(v $a commits_before)), so the scrub saw the write anyway"
		exit 2
	fi
done
c_b=$(v control b_scrubbed); c_p=$(v control platter_b)
case "$c_b" in
65536|eio|'?') echo "RESULT: INCONCLUSIVE -- the control's scrub lost nothing ('B' reads back as $c_b), so the fixed arms prove nothing"; exit 2;;
esac
for k in b_scrubbed platter_b b_remount; do
	[ "$(v commit $k)" = 65536 ] || {
		echo "RESULT: FAIL -- the committing scrub: $k is $(v commit $k), not 65536 bytes of 'B'"; exit 1; }
done
[ "$(v commit uncommitted)" = 0 ] || {
	echo "RESULT: FAIL -- the committing scrub declined $(v commit uncommitted) full stripe(s) all the same"; exit 1; }
[ "$(v commit marks_end)" = 0 ] || {
	echo "RESULT: FAIL -- the committing scrub left $(v commit marks_end) stale mark(s)"; exit 1; }
if [ "$(v decline uncommitted)" = 0 ] || [ "$(v decline uncommitted)" = '?' ]; then
	echo "RESULT: FAIL -- the scrub that could not see the write raised no scrub_uncommitted alert"; exit 1
fi
[ "$(v decline marks_scrubbed)" != 0 ] || {
	echo "RESULT: FAIL -- the declining scrub retired the record"; exit 1; }
for k in b_scrubbed platter_b b_remount; do
	[ "$(v decline $k)" = 65536 ] || {
		echo "RESULT: FAIL -- the declining scrub: $k is $(v decline $k), not 65536 bytes of 'B'"; exit 1; }
done
[ "$(v decline marks_end)" = 0 ] || {
	echo "RESULT: FAIL -- the scrub after the decline left $(v decline marks_end) stale mark(s)"; exit 1; }
echo "RESULT: PASS -- the scrub committed first and rebuilt 'B' onto the platter; without the commit it left"
echo "        the stripe and its record alone (scrub_uncommitted $(v decline uncommitted), state $(v decline state)) until a later scrub"
echo "        (control: parity regenerated from the stale column, 'B' reads back as $c_b of 65536 bytes, platter $c_p, silently)"
