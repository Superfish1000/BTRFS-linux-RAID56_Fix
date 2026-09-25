#!/bin/bash
# Does a read-modify-write served from the stripe cache lose a stale column?
#
#   rmw_cache.sh <kernel>
#
# See rmw_cache in init-final3.sh.  One boot per arm, RAID5 over four devices:
#   fixed    'B' reads back and is on its platter
#   control  raid56_rmw_trust_cache=1: 'B' is on neither
# The model predicted the loss (raid56_redundancy_model.py with the kernel's
# cache rule); this is it on a real kernel.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: rmw_cache.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-cache.sh.$$ && mv -f $T/umltest/init-cache.sh.$$ $T/umltest/init-cache.sh	# atomic: a guest may be reading it
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py
ulimit -c 0

arm() {	# name control
	local tag=rmw-cache-$1 d ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/results.$tag $T/umltest/rmw.cache.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-cache.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=rmw_cache OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 > $D/log 2>&1
	cat $T/umltest/rmw.cache.$tag 2>/dev/null || echo "? ?"
}
read -r f_read f_plat <<<"$(arm fixed 0)"
read -r c_read c_plat <<<"$(arm control 1)"
spl=$(grep -l KERNEL_SPLAT $T/umltest/rmw-cache-*/log 2>/dev/null | wc -l)
for a in fixed control; do grep -ah "layout:\|acknowledged\|refused\|RMW_CACHE" $T/umltest/rmw-cache-$a/log | sed "s/^/  [$a] /"; done
echo "  'B' bytes read back / on its platter (4096 each = intact): fixed $f_read/$f_plat, control $c_read/$c_plat, splats $spl"
case "$f_read$f_plat$c_read$c_plat" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
[ "$spl" -eq 0 ] || { echo "RESULT: FAIL -- kernel splat"; exit 1; }
if [ "$c_read" = 4096 ] && [ "$c_plat" = 4096 ]; then
	echo "RESULT: INCONCLUSIVE -- the control kept 'B', so a clean fix proves nothing"; exit 2
fi
[ "$f_read" = 4096 ] && [ "$f_plat" = 4096 ] || { echo "RESULT: FAIL -- 'B' lost with the fix"; exit 1; }
echo "RESULT: PASS -- the write into the recorded stripe read the disks and put"
echo "        'B' back; served from the cache, 'B' was lost"
