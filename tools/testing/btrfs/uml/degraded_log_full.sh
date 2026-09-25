#!/bin/bash
# Does a degraded RAID5/6 mount stay writable?
#
#   degraded_log_full.sh <kernel>
#
# See degraded_log_full in init-final3.sh.  RAID5 data and metadata, a device
# removed, mounted degraded, then small writes into more 4 MiB regions than
# the write-intent log holds.  Every one of them leaves a record naming the
# missing device, which nothing can retire while it is gone.
#   fixed    the log spends those records (with the record_dropped alert):
#            every write succeeds, metadata commits, the mount stays rw
#   control  raid56_keep_naming_degraded=1 keeps them: the log wedges
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: degraded_log_full.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-dlf.sh.$$ && mv -f $T/umltest/init-dlf.sh.$$ $T/umltest/init-dlf.sh	# atomic: a guest may be reading it
ulimit -c 0
arm() {	# name control
	local tag=dlf-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/dlf.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-dlf.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=degraded_log_full OPTS=rw PROFILE=raid5:raid5 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 NREG=${NREG:-120} < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
for a in fixed control; do
	grep -ah "mounted degraded\|writes ok\|DLF \|control:\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL" $T/umltest/dlf-$a/log | sed "s/^/  [$a] /"
done
read -r f_ok f_eio f_meta f_ro f_drop f_full < $T/umltest/dlf.dlf-fixed 2>/dev/null || f_ok=?
read -r c_ok c_eio c_meta c_ro c_drop c_full < $T/umltest/dlf.dlf-control 2>/dev/null || c_ok=?
case "$f_ok$c_ok" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/dlf-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
if [ "$c_eio" = 0 ] && [ "$c_ro" = 0 ] && [ "$c_meta" = 1 ]; then
	echo "RESULT: INCONCLUSIVE -- the control did not wedge, so a clean fixed arm proves nothing"; exit 2
fi
if [ "$f_eio" != 0 ] || [ "$f_ro" != 0 ] || [ "$f_meta" != 1 ]; then
	echo "RESULT: FAIL -- the degraded mount wedged: eio=$f_eio ro=$f_ro meta=$f_meta"; exit 1
fi
[ "${f_drop:-0}" -ge 1 ] || { echo "RESULT: FAIL -- records were spent without the record_dropped alert"; exit 1; }
echo "RESULT: PASS -- degraded, every write succeeded and metadata committed (record_dropped $f_drop);"
echo "        keeping the records wedged it: eio=$c_eio ro=$c_ro meta=$c_meta"
