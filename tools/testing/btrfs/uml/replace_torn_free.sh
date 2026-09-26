#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# A crash tore a RAID5 full stripe whose free data column is on the device
# that then goes missing: does the replace of that device rebuild the free
# sectors onto the new device, or count them as lost?
#
#   replace_torn_free.sh <kernel>
#
# See replace_torn_free in init-final3.sh.  RAID5 data, RAID1C3 metadata over
# four devices, a fifth the replace target.  A 128 KiB nodatacow file fills
# columns 0 and 1 of the first full stripe; an in-place write into column 1
# is torn by raid56_crash_point=1.  The next boot leaves out the device of
# column 2, which holds no extent, mounts degraded (the recovery keeps the
# stripe marked possibly torn) and replaces that device.
#   fixed    the replace rebuilds column 2 onto the new device (row 0 not
#            zero there), counts nothing uncopyable, alerts nothing
#            (replace_uncopyable, read_unrecovered 0) and leaves no stale mark
#   control  raid56_wf_replace_refuses_free=1: the rebuild of the free
#            sectors is refused as data rebuilt from a possibly torn parity;
#            they are counted lost (replace_uncopyable, 'data there is lost'),
#            the new device holds zeros there and the column is recorded stale
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: replace_torn_free.sh <kernel>}
NDEV=5
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-rtfree.sh.$$ &&
	mv -f $T/umltest/init-rtfree.sh.$$ $T/umltest/init-rtfree.sh
cp $HERE/raid56_rows.py $T/umltest/raid56_rows.py.$$ &&
	mv -f $T/umltest/raid56_rows.py.$$ $T/umltest/raid56_rows.py
ulimit -c 0
arm() {	# name control
	local tag=rtfree-$1 ubds="" ph
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/rtf.$tag $T/umltest/rtf.*.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do
		truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"
	done
	for ph in 1 2; do
		timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-rtfree.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=replace_torn_free OPTS=rw TAG=$tag NDEV=4 \
			PHASE=$ph CONTROL=$2 < /dev/null > $D/log.$ph 2>&1
		echo "boot $ph rc=$?" >> $D/log.$ph
	done
	cat $D/log.1 $D/log.2 > $D/log
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
SHOW="layout:|crash armed|NO_CRASH|RTF |replace rc|replace status|could neither copy"
SHOW="$SHOW|refusing a read|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|LAYOUT_FAIL"
SHOW="$SHOW|DM_REMOVE|KNOB_FAIL|CRASH_ARM_FAIL"
for a in fixed control; do
	grep -ahE "$SHOW" $T/umltest/rtfree-$a/log | cut -c1-300 | head -20 | sed "s/^/  [$a] /"
done
# replace rc, uncorrectable read errors, replace_uncopyable, read_unrecovered,
# stale_marks, non-zero bytes of the target's row 0 of the free column,
# file blocks read ok / not
res() { cat $T/umltest/rtf.rtfree-$1 2>/dev/null || echo "? ? ? ? ? ? ? ?"; }
read -r f_rrc f_unc f_lost f_unrec f_stale f_nz f_ok f_bad <<<"$(res fixed)"
read -r c_rrc c_unc c_lost c_unrec c_stale c_nz c_ok c_bad <<<"$(res control)"
echo "  replace rc, uncorr. read errs, replace_uncopyable, read_unrecovered, stale_marks," \
     "target row 0 non-zero bytes, file blocks ok/bad:"
echo "    fixed   $f_rrc $f_unc $f_lost $f_unrec $f_stale $f_nz $f_ok/$f_bad"
echo "    control $c_rrc $c_unc $c_lost $c_unrec $c_stale $c_nz $c_ok/$c_bad"
case "$f_rrc$c_rrc" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
if grep -lq KERNEL_SPLAT $T/umltest/rtfree-*/log.2; then
	echo "RESULT: FAIL -- kernel splat"
	exit 1
fi
for a in fixed control; do
	if ! grep -q "crash armed" $T/umltest/rtfree-$a/log.1 ||
	   grep -q "NO_CRASH\|LAYOUT_FAIL\|KNOB_FAIL\|DM_REMOVE_FAIL" $T/umltest/rtfree-$a/log; then
		echo "RESULT: INCONCLUSIVE -- $a: no torn write, or the layout or a knob failed"
		exit 2
	fi
	if ! grep -q "RTF after the recovery: torn_blocks=[1-9]" $T/umltest/rtfree-$a/log.2; then
		echo "RESULT: INCONCLUSIVE -- $a: the recovery kept no stripe marked possibly torn"
		exit 2
	fi
done
if [ "$f_bad" != 0 ] || [ "$c_bad" != 0 ]; then
	echo "RESULT: FAIL -- the file did not read back ($f_bad, control $c_bad blocks)"
	exit 1
fi
if [ "${c_lost:-0}" = 0 ] || [ "${c_nz:-1}" != 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control did not count the free sectors lost" \
	     "(replace_uncopyable $c_lost, target row 0 $c_nz non-zero bytes)"
	exit 2
fi
if [ "$f_rrc" != 0 ] || [ "${f_lost:-1}" != 0 ] || [ "${f_unc:-1}" != 0 ] ||
   [ "${f_unrec:-1}" != 0 ] || [ "${f_stale:-1}" != 0 ]; then
	echo "RESULT: FAIL -- the replace counted, alerted or recorded free sectors as lost"
	exit 1
fi
if [ "$f_nz" != 4096 ]; then
	echo "RESULT: FAIL -- the new device does not hold the rebuild of the free column" \
	     "($f_nz non-zero bytes in row 0)"
	exit 1
fi
echo "RESULT: PASS -- the replace rebuilt the free column onto the new device and counted,"
echo "        alerted and recorded nothing; refused, $c_unc sectors were counted lost,"
echo "        replace_uncopyable $c_lost, the new device held zeros"
