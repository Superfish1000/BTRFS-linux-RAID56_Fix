#!/bin/bash
# Does the queued RAID5/6 repair write to a frozen filesystem?
#
#   repair_freeze.sh <kernel>
#
# See repair_freeze in init-final3.sh.  Two arms, one boot each:
#   fixed    0 device writes and 0 repairs while frozen; repairs after the thaw
#   control  raid56_repair_ignores_freeze=1: the repair writes while frozen
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: repair_freeze.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-freeze.sh
ulimit -c 0
arm() {	# name control
	local tag=repair-freeze-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/repair.freeze.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-freeze.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=repair_freeze OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV FAIL=1 CONTROL=$2 > $D/log 2>&1
	cat $T/umltest/repair.freeze.$tag 2>/dev/null || echo "? ? ?"
}
read -r fw fr fa <<<"$(arm fixed 0)"
read -r cw cr ca <<<"$(arm control 1)"
spl=$(grep -l KERNEL_SPLAT $T/umltest/repair-freeze-*/log 2>/dev/null | wc -l)
echo "  while frozen: device writes / repairs; repairs after thaw"
echo "  fixed   $fw / $fr ; $fa"
echo "  control $cw / $cr ; $ca    splats $spl"
case "$fw$fr$fa$cw$cr$ca" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
[ "$spl" -eq 0 ] || { echo "RESULT: FAIL -- kernel splat"; exit 1; }
[ "$cr" -gt 0 ] || { echo "RESULT: INCONCLUSIVE -- the control did not repair while frozen either"; exit 2; }
[ "$fw" -eq 0 ] && [ "$fr" -eq 0 ] || { echo "RESULT: FAIL -- $fw write(s), $fr repair(s) on a frozen filesystem"; exit 1; }
[ "$fa" -gt 0 ] || { echo "RESULT: FAIL -- the repairs never ran after the thaw"; exit 1; }
echo "RESULT: PASS -- nothing written while frozen, $fa repair(s) after the thaw;"
echo "        ignoring the freeze, the control repaired $cr time(s) and wrote $cw block(s) while frozen"
