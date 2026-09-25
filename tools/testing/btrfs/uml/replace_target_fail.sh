#!/bin/bash
# Does a device replace notice a target that does not take its writes?
#
#   replace_target_fail.sh <kernel>
#
# See replace_target_fail in init-final3.sh.
#   fixed    the replace fails (EIO); the source stays in the filesystem
#   control  scrub_replace_ignores_write_errors=1: the replace "finishes",
#            the target joins with holes, and a scrub finds them
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: replace_target_fail.sh <kernel>}
NDEV=5
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-rtf.sh.$$ && mv -f $T/umltest/init-rtf.sh.$$ $T/umltest/init-rtf.sh	# atomic: a guest may be reading it
ulimit -c 0
arm() {	# name control
	local tag=rtf-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/rtf.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-rtf.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=replace_target_fail OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
for a in fixed control; do
	grep -ah "target \|replace rc\|replace status\|in use\|scrub:\|RTF \|control:\|could not write\|KERNEL_SPLAT\|MOUNT_FAIL\|MKFS_FAIL\|DM_RELOAD" $T/umltest/rtf-$a/log | sed "s/^/  [$a] /"
done
read -r f_rc f_in f_md5 f_csum < $T/umltest/rtf.rtf-fixed 2>/dev/null || f_rc=?
read -r c_rc c_in c_md5 c_csum < $T/umltest/rtf.rtf-control 2>/dev/null || c_rc=?
case "$f_rc$c_rc" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/rtf-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
if [ "$c_rc" != 0 ] || [ "$c_in" != 1 ]; then
	echo "RESULT: INCONCLUSIVE -- the control's replace did not finish, so the setup failed it some other way"; exit 2
fi
[ "$c_csum" -gt 0 ] || echo "  note: the control's scrub found no checksum errors"
if [ "$f_rc" = 0 ] || [ "$f_in" != 0 ]; then
	echo "RESULT: FAIL -- the replace finished onto a target that did not take its writes"; exit 1
fi
[ "$f_md5" = 1 ] || { echo "RESULT: FAIL -- the data does not read back after the refused replace"; exit 1; }
echo "RESULT: PASS -- the replace failed and the source stayed; ignoring the errors it finished"
echo "        onto a target with holes (scrub csum errors $c_csum)"
