#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does the recovery's verdict on a torn stripe it cannot decide stay the
# verdict -- a stale parity in memory, spent last by a full log -- after a
# device replace ends, and across a remount?
#
#   verdict_keep.sh <kernel> [replace|remount|flush]
#
# See verdict_keep in init-final3.sh.  The disks torn_readd.sh's upgrade arm
# prepares (RAID5 data, RAID1C3 metadata, four devices; every error record of
# the log possibly torn), plus a preallocated nodatacow file for writes into
# new regions.  Device 2 is left out and the array mounted degraded
# read-write: the recovery records ~70 stripes undecidable, in a narrow log of
# ~97 regions, and their rows on device 2 read EIO.  Then:
#   replace  device 4 (present) is replaced by a fifth, and it finishes
#     fixed    the verdicts stay: the writes into new regions that follow
#              succeed, spending other records, and every row reads EIO
#     control  raid56_wf_replace_end_clears_verdicts=1: the replace's end
#              turns them into ordinary stale parities, the log no longer
#              fits a block ("block full"), writes fail until records are
#              spent, and the ones spent are the verdicts: rows read wrong
#   remount  a few writes make the log block wide (it carries the verdicts as
#            stale parities), unmount, mount degraded again
#     fixed    the reloaded verdicts are spent last: the writes into new
#              regions spend other records, every row reads EIO
#     control  raid56_wf_reload_verdicts_plain=1: they reload as ordinary
#              stale parities and are spent in table order: rows of the
#              spent ones are rebuilt from the torn parity and returned --
#              right by chance (ok) or wrong, never refused
#   flush    as remount, then device 0 (present) fails its flushes while a
#            block is written into another full stripe of each row's region
#            and finishes; a write into a new region makes the log flush,
#            which fails: the readd takes those blocks back, and with them
#            the stale records the last block carried for the same regions,
#            the reloaded verdicts among them.  Device 0 is healed.
#     fixed    the verdicts stay verdicts, spent last: every row reads EIO
#     control  raid56_wf_readd_disowns_all=1: the readd takes them over as
#              plain stale parities, a full log spends them in table order,
#              and their rows are rebuilt from the torn parity and returned
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: verdict_keep.sh <kernel> [replace|remount|flush]}
ARM=${2:-replace}
case $ARM in replace|remount|flush) ;; *) echo "unknown arm $ARM"; exit 2;; esac
NDEV=4
FAIL=1
OMIT=2
VK_SPARE=48
VK_WRITES=40
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-vk.sh.$$ &&
	mv -f $T/umltest/init-vk.sh.$$ $T/umltest/init-vk.sh
cp $HERE/raid56_rows.py $T/umltest/raid56_rows.py.$$ &&
	mv -f $T/umltest/raid56_rows.py.$$ $T/umltest/raid56_rows.py
ulimit -c 0
arm() {	# name control
	local tag=vk-$ARM-$1 control=$2
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/tr-prep.$tag $T/umltest/vk.$tag.* $T/umltest/tr.rows.$tag \
	      $T/umltest/tr.acked.$tag $T/umltest/results.$tag
	for i in $(seq 0 $NDEV); do truncate -s 1G $D/disk$i.img; done
	boot() {	# mode phase omit opts control
		local ubds="" mnt="" d
		for d in $(seq 0 $((NDEV-1))); do
			[ "$d" = "$3" ] && continue
			ubds="$ubds ubd$d=$D/disk$d.img"
			[ -n "$mnt" ] || mnt=/dev/ubd$(echo abcdefgh | cut -c$((d + 1)))
		done
		# The replace target, only once the array is degraded.
		[ "$1" = verdict_keep ] && ubds="$ubds ubd$NDEV=$D/disk$NDEV.img"
		timeout 1800 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-vk.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$1 OPTS=$4 PROFILE=raid5:raid1c3 TAG=$tag \
			NDEV=$NDEV FAIL=$FAIL CONTROL=$5 MNTDEV=$mnt ARM=$ARM \
			OMITTED=$([ "$3" = none ] || echo "$3") PHASE=$2 VK_SPARE=$VK_SPARE \
			VK_WRITES=$VK_WRITES REPLACE=$NDEV \
			TGT=/dev/ubd$(echo abcdefgh | cut -c$((NDEV + 1))) \
			< /dev/null > $D/log.$2 2>&1
		echo "boot $1 $2 rc=$?" >> $D/log.boots
	}
	# The upgrade prep of torn_readd.sh: its log marks nothing, so every
	# error record of it is possibly torn at the next mount.
	boot torn_readd_prep prep none rw,commit=600 2
	if [ $ARM = replace ]; then
		boot verdict_keep replace $OMIT rw $control
	elif [ $ARM = remount ]; then
		boot verdict_keep wide $OMIT rw $control
		boot verdict_keep read $OMIT rw $control
	else
		# No transaction commit while device 0 fails its flushes.
		boot verdict_keep wide $OMIT rw $control
		boot verdict_keep read $OMIT rw,commit=600 $control
	fi
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
SHOW="control:|VK |replace rc|replace status|block full|lazy commit|still full|log full"
SHOW="$SHOW|did not confirm|not waiting"
SHOW="$SHOW|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|LAYOUT_FAIL|DM_RELOAD|KNOB_FAIL"
SHOW="$SHOW|FALLOCATE_FAIL"
for a in fixed control; do
	grep -ahE "$SHOW" $T/umltest/vk-$ARM-$a/log.* | grep -v "TR_WRONG\|TR_DIRECT_BAD" |
		head -40 | sed "s/^/  [$a] /"
done
P=$([ $ARM = replace ] && echo replace || echo read)
res() { cat $T/umltest/vk.vk-$ARM-$1.$P 2>/dev/null || echo "? ? ? ? ? ? ? ? ?"; }
read -r f_chk f_ok f_eio f_wrong f_dbad f_failed f_full f_ev f_rrc <<<"$(res fixed)"
read -r c_chk c_ok c_eio c_wrong c_dbad c_failed c_full c_ev c_rrc <<<"$(res control)"
echo "  device $OMIT's blocks of the rewritten rows, ok/eio/wrong of checked;" \
     "writes failed, block full warnings, records spent, replace rc:"
echo "    fixed   $f_ok/$f_eio/$f_wrong of $f_chk; $f_failed, $f_full, $f_ev, $f_rrc"
echo "    control $c_ok/$c_eio/$c_wrong of $c_chk; $c_failed, $c_full, $c_ev, $c_rrc"
case "$f_chk$c_chk" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/vk-$ARM-*/log.* && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/vk-$ARM-*/log.* && {
	echo "RESULT: INCONCLUSIVE -- a raid56_wf_* knob is not there"
	echo "        (not a CONFIG_BTRFS_DEBUG kernel?)"; exit 2
}
if [ "$f_eio" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- no row read EIO: the recovery recorded no verdict"
	exit 2
fi
if [ $ARM = flush ]; then
	read -r f_tf f_trig f_fl < $T/umltest/vk.vk-flush-fixed.flush 2>/dev/null
	read -r c_tf c_trig c_fl < $T/umltest/vk.vk-flush-control.flush 2>/dev/null
	echo "  writes into the rows' regions failed, the new region's, log flushes failed:" \
	     "fixed ${f_tf:-?} ${f_trig:-?} ${f_fl:-?}, control ${c_tf:-?} ${c_trig:-?} ${c_fl:-?}"
	if [ "${f_fl:-0}" = 0 ] || [ "${c_fl:-0}" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the log flush did not fail (fixed ${f_fl:-?}," \
		     "control ${c_fl:-?}): no readd"
		exit 2
	fi
	if [ "$f_wrong" != 0 ] || [ "$f_dbad" != 0 ] || [ "$f_ok" != 0 ]; then
		echo "RESULT: FAIL -- rows of undecided stripes were rebuilt with the verdicts kept:" \
		     "ok $f_ok, wrong $f_wrong, other devices $f_dbad"
		exit 1
	fi
	if [ "$f_failed" != 0 ]; then
		echo "RESULT: FAIL -- $f_failed writes failed after device 0 was healed"
		exit 1
	fi
	if [ $((c_ok + c_wrong)) = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the control spent no verdict either, so a clean"
		echo "        fixed arm proves nothing"
		exit 2
	fi
	echo "RESULT: PASS -- after a failed log flush's readd the verdicts were spent last (eio"
	echo "        $f_eio, 0 rebuilt); taken over by the readd, $((c_ok + c_wrong)) were spent and"
	echo "        their rows rebuilt from the torn parity, $c_wrong of them wrong"
	exit 0
fi
if [ $ARM = replace ]; then
	[ "$f_rrc" = 0 ] && [ "$c_rrc" = 0 ] || {
		echo "RESULT: INCONCLUSIVE -- a replace did not finish" \
		     "(fixed $f_rrc, control $c_rrc)"
		exit 2
	}
else
	read -r w_failed w_ev w_marks < $T/umltest/vk.vk-remount-fixed.wide 2>/dev/null
	if [ "${w_ev:-0}" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the writes of the wide phase spent no record:"
		echo "        nothing says the block went wide (stale_marks ${w_marks:-?})"
		exit 2
	fi
fi
if [ "$f_wrong" != 0 ] || [ "$f_dbad" != 0 ]; then
	echo "RESULT: FAIL -- rows read back wrong with the verdicts kept:" \
	     "missing column $f_wrong, other devices $f_dbad"
	exit 1
fi
if [ "$f_failed" != 0 ] || [ "$f_full" != 0 ]; then
	echo "RESULT: FAIL -- writes failed ($f_failed) or the log did not fit a block" \
	     "($f_full warnings) with the verdicts kept"
	exit 1
fi
# Every row checked is on a stripe the recovery could not decide: a verdict
# kept refuses it, one spent returns a rebuild, right or wrong by chance.  The
# replace copies a device that is there and decides none of them.
if [ "$f_ok" != 0 ]; then
	echo "RESULT: FAIL -- $f_ok rows of undecided stripes were rebuilt with the verdicts kept"
	exit 1
fi
if [ $ARM = replace ] && [ "$c_full" = 0 ] && [ "$c_failed" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control's log still fit a block after the replace"
	exit 2
fi
if [ $ARM = remount ] && [ $((c_ok + c_wrong)) = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control spent no verdict either, so a clean"
	echo "        fixed arm proves nothing"
	exit 2
fi
if [ $ARM = replace ]; then
	echo "RESULT: PASS -- after the replace the verdicts stayed and the log stayed writable"
	echo "        (eio $f_eio, 0 rebuilt); released with the replace's marks, the log no longer"
	echo "        fit a block: $c_failed writes failed ($c_full 'block full'), and $c_wrong rows"
	echo "        read wrong"
	exit 0
fi
echo "RESULT: PASS -- the verdicts reloaded from a wide block were spent last (eio $f_eio,"
echo "        0 rebuilt); reloaded as plain stale parities, $((c_ok + c_wrong)) were spent and"
echo "        their rows rebuilt from the torn parity, $c_wrong of them wrong"
