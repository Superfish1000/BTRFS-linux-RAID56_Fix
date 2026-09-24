#!/bin/bash
# Does a crash in the write that repairs a stale column lose that column?
#
#   rmw_torn.sh <kernel>
#
# See rmw_torn in init-final3.sh.  Two boots per arm: the one that crashes,
# and a read-write mount after it (recovery runs), which then reads 'B' and
# checks its platter.
#   fixed    two-phase: 'B' intact
#   control  raid56_rmw_single_phase=1: recovery rebuilds 'B' from the torn
#            parity and writes the result back
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: rmw_torn.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-torn.sh
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py
ulimit -c 0
arm() {	# name control
	local tag=rmw-torn-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/rmw.torn.$tag $T/umltest/layout.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	for m in rmw_torn rmw_torn_verify; do
		timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-torn.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$m OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
			NDEV=$NDEV CONTROL=$2 > $D/log.$m 2>&1
	done
	cat $T/umltest/rmw.torn.$tag 2>/dev/null || echo "? ?"
}
read -r f_read f_plat <<<"$(arm fixed 0)"
read -r c_read c_plat <<<"$(arm control 1)"
spl=$(grep -l KERNEL_SPLAT $T/umltest/rmw-torn-*/log.* 2>/dev/null | wc -l)
for a in fixed control; do
	grep -ah "acknowledged\|refused\|crash armed\|NO_CRASH\|crash injection\|RMW_TORN" $T/umltest/rmw-torn-$a/log.* | sed "s/^/  [$a] /"
done
echo "  'B' bytes read back / on its platter after the crash and recovery: fixed $f_read/$f_plat, control $c_read/$c_plat, splats $spl"
case "$f_read$f_plat$c_read$c_plat" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -q "crash injection 2" $T/umltest/rmw-torn-fixed/log.rmw_torn 2>/dev/null ||
	{ echo "RESULT: INCONCLUSIVE -- the fixed arm never crashed"; exit 2; }
[ "$spl" -eq 0 ] || { echo "RESULT: FAIL -- kernel splat"; exit 1; }
if [ "$c_read" = 4096 ] && [ "$c_plat" = 4096 ]; then
	echo "RESULT: INCONCLUSIVE -- the control kept 'B', so a clean fix proves nothing"; exit 2
fi
[ "$f_read" = 4096 ] && [ "$f_plat" = 4096 ] || { echo "RESULT: FAIL -- 'B' lost with the fix"; exit 1; }
echo "RESULT: PASS -- 'B' survived a crash in the write that repaired it; with the"
echo "        write-back in the same batch as the parity, recovery lost it"
