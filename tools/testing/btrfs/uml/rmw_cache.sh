#!/bin/bash
# Does a read-modify-write served from the stripe cache lose a stale column?
#
#   rmw_cache.sh <kernel>
#
# See rmw_cache in init-final3.sh.  One boot per arm, RAID5 over four devices:
#   fixed         'B' reads back and is on its platter
#   control       raid56_rmw_trust_cache=1: 'B' is on neither
#   name-control  raid56_wf_name_unwritten=1: 'B' is on its platter but its
#                 read is refused.  C's repair-first persist fails its flush
#                 on the parity device, and the readd names that parity stale
#                 in B's stripe although nothing was written to it since the
#                 fsync's barrier flushed it: two stale members under one
#                 parity.  Fixed, it names only what a write went to.  The
#                 arm shows that only if 'B' is on its platter and its read
#                 fails; a read that returns anything but 'B' without an
#                 error is a wrong read, and fails the test.
# The model predicted the loss (raid56_redundancy_model.py with the kernel's
# cache rule); this is it on a real kernel.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: rmw_cache.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-cache.sh.$$ && mv -f $T/umltest/init-cache.sh.$$ $T/umltest/init-cache.sh	# atomic: a guest may be reading it
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py.$$ &&
	mv -f $T/umltest/raid56_layout.py.$$ $T/umltest/raid56_layout.py
ulimit -c 0

arm() {	# name control [kernel args...]
	local tag=rmw-cache-$1 control=$2 d ubds=""
	shift 2
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/results.$tag $T/umltest/rmw.cache.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-cache.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=rmw_cache OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV CONTROL=$control "$@" > $D/log 2>&1
	cat $T/umltest/rmw.cache.$tag 2>/dev/null || echo "? ? ?"
}
# Each: 'B' bytes read back, 'B' bytes on its platter, 1 if the read failed.
read -r f_read f_plat f_rc <<<"$(arm fixed 0)"
read -r c_read c_plat c_rc <<<"$(arm control 1)"
read -r n_read n_plat n_rc <<<"$(arm name-control 0 btrfs.raid56_wf_name_unwritten=1)"
spl=$(grep -l KERNEL_SPLAT $T/umltest/rmw-cache-*/log 2>/dev/null | wc -l)
for a in fixed control name-control; do
	grep -ah "layout:\|acknowledged\|refused\|RMW_CACHE\|did not confirm\|refusing a read" \
		$T/umltest/rmw-cache-$a/log | sed "s/^/  [$a] /"
done
echo "  'B' bytes read back / on its platter (4096 each = intact): fixed $f_read/$f_plat," \
     "control $c_read/$c_plat, name-control $n_read/$n_plat, splats $spl"
echo "  read failed (1) or returned data (0): fixed ${f_rc:-?}, control ${c_rc:-?}," \
     "name-control ${n_rc:-?}"
case "$f_read$f_plat$c_read$c_plat$n_read$n_plat${n_rc:-?}" in
*'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;;
esac
[ "$spl" -eq 0 ] || { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -aq "knob raid56_wf_name_unwritten=Y" $T/umltest/rmw-cache-name-control/log || {
	echo "RESULT: INCONCLUSIVE -- raid56_wf_name_unwritten is not set in the name-control arm"
	echo "        (not a CONFIG_BTRFS_DEBUG kernel, or one without the knob)"; exit 2
}
[ "$f_read" = 4096 ] && [ "$f_plat" = 4096 ] || {
	echo "RESULT: FAIL -- 'B' lost or refused with the fix" \
	     "(read $f_read, platter $f_plat)"; exit 1
}
if [ "$c_read" = 4096 ] && [ "$c_plat" = 4096 ]; then
	echo "RESULT: INCONCLUSIVE -- the control kept 'B', so a clean fix proves nothing"; exit 2
fi
if [ "$n_rc" = 0 ] && [ "$n_read" != 4096 ]; then
	echo "RESULT: FAIL -- with the unwritten parity named, the read of 'B' returned"
	echo "        something else ($n_read bytes 'B') without an error"; exit 1
fi
if [ "$n_read" = 4096 ]; then
	echo "RESULT: INCONCLUSIVE -- naming the unwritten parity did not refuse 'B' either,"
	echo "        so the fixed arm's read proves nothing about the naming"; exit 2
fi
if [ "$n_plat" != 4096 ]; then
	echo "RESULT: INCONCLUSIVE -- the name-control arm did not leave 'B' on its platter"
	echo "        ($n_plat bytes), so its failed read is not the refusal it is there for"
	exit 2
fi
echo "RESULT: PASS -- the write into the recorded stripe read the disks and put"
echo "        'B' back; served from the cache, 'B' was lost; naming the parity"
echo "        nothing had written to refused the read of 'B', which is on its platter"
