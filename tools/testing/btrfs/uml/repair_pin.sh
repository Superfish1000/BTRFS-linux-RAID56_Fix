#!/bin/bash
# Can a repair write into device space its block group no longer owns?
#
#   repair_pin.sh <kernel>
#
# See repair_pin in init-final3.sh.  One boot per arm:
#   fixed    the balance waits for the repair; the new data stays intact
#   control  raid56_repair_no_pin=1: the repair lands in the refilled space
#
# The failing device is devid 4 (FAIL=3): the stale column lands there, and in
# this layout the freed space on devids 3 and 4 is reused by the refill, where
# devid 2's hole is smaller than a new chunk and stays free -- a stray write
# into free space damages nothing and proves nothing.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: repair_pin.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-pin.sh
ulimit -c 0
arm() {	# name control
	local tag=repair-pin-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/repair.pin.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 2400 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-pin.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=repair_pin OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV FAIL=${FAIL:-3} CONTROL=$2 HOLD=${HOLD:-150000} > $D/log 2>&1
	cat $T/umltest/repair.pin.$tag 2>/dev/null || echo "? ? ?"
}
read -r fh fok fe <<<"$(arm fixed 0)"
read -r ch cok ce <<<"$(arm control 1)"
spl=$(grep -l KERNEL_SPLAT $T/umltest/repair-pin-fixed/log 2>/dev/null | wc -l)
for a in fixed control; do grep -ah "holding:\|balance rc\|refilled\|REPAIR_PIN\|still recorded" $T/umltest/repair-pin-$a/log | sed "s/^/  [$a] /"; done
echo "  held / new data read back intact / damage found (csum failures + corruption counts + parity mismatches): fixed $fh/$fok/$fe, control $ch/$cok/$ce, splats (fixed) $spl"
case "$fh$fok$ch$cok" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
[ "$fh" = yes ] && [ "$ch" = yes ] || { echo "RESULT: INCONCLUSIVE -- no repair was held, so nothing was in flight"; exit 2; }
[ "$spl" -eq 0 ] || { echo "RESULT: FAIL -- kernel splat"; exit 1; }
# Damage is the new data reading back wrong, or its parity no longer
# matching it -- where a stray write lands decides which.
if [ "$cok" = 1 ] && [ "${ce:-0}" -eq 0 ] 2>/dev/null; then
	echo "RESULT: INCONCLUSIVE -- the unpinned control did not damage the new data"; exit 2
fi
[ "$fok" = 1 ] && [ "${fe:-1}" -eq 0 ] 2>/dev/null ||
	{ echo "RESULT: FAIL -- the new data or its parity was damaged with the repair pinned"; exit 1; }
echo "RESULT: PASS -- pinned, the repair finished before its space was given away;"
echo "        unpinned, it wrote into the new data"
