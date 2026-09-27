#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does the write-intent log say which device may have lost a write when that
# device is detached while the filesystem is mounted, and comes back?
#
#   detach_flush.sh <kernel>
#
# See detach_flush in init-final3.sh.  RAID5 over three null_blk devices,
# memory-backed with a volatile write cache (the kernel needs
# CONFIG_BLK_DEV_NULL_BLK and CONFIG_CONFIGFS_FS: build it apart from the one
# the other scenarios run on, see README.md), RAID1 metadata, nodatasum.
# With no flush, one block per full stripe of a preallocated nodatacow file is
# written in place (the log records it) and a new file is written with
# O_DIRECT in full stripes (it does not); then device 1 is powered off --
# detached, its cache lost -- and a sync commits, its barrier leaving the
# missing device out.  Unmounted, device 1 powered on again, mounted with
# every device, both files read back.
#   fixed     the commit counts the detached device as one that did not
#             confirm the flush (btrfs_wib_device_lost()): the log names what
#             was written to it, and every block reads back as written
#   control   raid56_wf_detach_unnamed=1: nothing is named; the blocks of the
#             detached device's column read back as they were before the
#             write (zeros), with no error
#   inflight  the fixed kernel, with raid56_wf_load_stale_inflight=1 at the
#             last mount: device 1's own last log block, older than the
#             others', still lists the overwrites in flight; taken at its
#             word, the recovery takes those stripes for possibly torn and
#             refuses the named column (EIO) -- refused, not wrong, but
#             refused for nothing: the newer block records the writes
#             finished and names their damage (wib_load_block())
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: detach_flush.sh <kernel>}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-df.sh.$$ && mv -f $T/umltest/init-df.sh.$$ $T/umltest/init-df.sh
ulimit -c 0
arm() {	# name control
	local tag=df-$1
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/df.$tag $T/umltest/results.$tag
	# null_blk.nr_devices=0: only the devices the guest configures.
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw null_blk.nr_devices=0 \
		init=$T/umltest/init-df.sh quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=detach_flush TAG=$tag NDEV=3 DF_DEV=1 CONTROL=$2 \
		< /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
}
arm fixed 0
arm control 1
arm inflight 2
ARMS="fixed control inflight"
SHOW="control:|overwrites:|gone missing|was detached|DF |DF_|NULLB_|did not confirm"
SHOW="$SHOW|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|KNOB_FAIL"
for a in $ARMS; do
	grep -ahE "$SHOW" $T/umltest/df-$a/log | cut -c1-260 | sed "s/^/  [$a] /"
done
# The value of key $2 in arm $1's report, or ?.
v() { sed -n "s/.*\<$2=\([^ ]*\).*/\1/p" $T/umltest/df.df-$1 2>/dev/null | head -1 | grep . ||
	echo '?'; }
grep -lq NULLB_UNAVAILABLE $T/umltest/df-*/log &&
	{ echo "RESULT: INCONCLUSIVE -- no null_blk or configfs in this kernel"; exit 2; }
for a in $ARMS; do
	[ -s $T/umltest/df.df-$a ] || { echo "RESULT: INCONCLUSIVE -- the $a arm did not report"; exit 2; }
done
grep -lq KERNEL_SPLAT $T/umltest/df-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq WATCHDOG $T/umltest/df-*/log && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/df-control/log $T/umltest/df-inflight/log &&
	{ echo "RESULT: INCONCLUSIVE -- a raid56_wf_* knob is not there (not a CONFIG_BTRFS_DEBUG kernel?)"
	  exit 2; }
grep -lq "DF_NOT_MISSING\|NULLB_FAIL\|DF_MOUNT_FAIL" $T/umltest/df-*/log &&
	{ echo "RESULT: INCONCLUSIVE -- the device was not detached, did not come back, or the"
	  echo "        filesystem did not mount with it"; exit 2; }
for a in $ARMS; do
	if [ "$(v $a commits_while_cached)" != 0 ]; then
		echo "RESULT: INCONCLUSIVE -- a transaction commit ran before the detach ($a)"; exit 2
	fi
	if [ "$(v $a acked)" = 0 ] || [ "$(v $a acked)" = '?' ]; then
		echo "RESULT: INCONCLUSIVE -- no overwrite acknowledged ($a)"; exit 2
	fi
done
echo "  after the commit: stale_marks fixed $(v fixed stale_marks), control $(v control stale_marks);" \
     "sync fixed $(v fixed sync_ok), control $(v control sync_ok)"
for a in $ARMS; do
	echo "  [$a] overwritten blocks new/old/eio/other $(v $a b_new)/$(v $a b_old)/$(v $a b_eio)/$(v $a b_other)," \
	     "new file 64 KiB chunks $(v $a c_new)/$(v $a c_old)/$(v $a c_eio)/$(v $a c_other)"
done
c_old=$(( $(v control b_old) + $(v control c_old) ))
if [ "$c_old" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control read nothing back old, so a clean fixed arm proves nothing"
	exit 2
fi
if [ "$(v fixed sync_ok)" != 1 ] || [ "$(v fixed ro)" != 0 ]; then
	echo "RESULT: FAIL -- the commit after the detach did not go on (sync $(v fixed sync_ok), ro $(v fixed ro))"
	exit 1
fi
if [ "$(v fixed stale_marks)" = 0 ]; then
	echo "RESULT: FAIL -- the log named nothing after the detach"; exit 1
fi
bad=$(( $(v fixed b_old) + $(v fixed b_other) + $(v fixed c_old) + $(v fixed c_other) ))
if [ "$bad" != 0 ]; then
	echo "RESULT: FAIL -- $bad block(s) read back wrong without an error after the device came back"
	exit 1
fi
# The names say exactly what the device lost, and the parity the others
# flushed rebuilds it: a refusal would be for nothing.
eio=$(( $(v fixed b_eio) + $(v fixed c_eio) ))
if [ "$eio" != 0 ]; then
	echo "RESULT: FAIL -- $eio block(s) the names prove failed to read (EIO)"; exit 1
fi
# The inflight arm refuses (the names are there, the listing says torn): it
# must never read anything wrong, and it must refuse something, or the fixed
# arm's clean reads do not show what leaving that listing out is for.
i_bad=$(( $(v inflight b_old) + $(v inflight b_other) + $(v inflight c_old) + $(v inflight c_other) ))
if [ "$i_bad" != 0 ]; then
	echo "RESULT: FAIL -- with raid56_wf_load_stale_inflight=1, $i_bad block(s) read back wrong"
	exit 1
fi
i_eio=$(( $(v inflight b_eio) + $(v inflight c_eio) ))
if [ "$i_eio" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- with raid56_wf_load_stale_inflight=1 nothing was refused either,"
	echo "        so the fixed arm's clean reads do not show the older listing left out"
	exit 2
fi
echo "RESULT: PASS -- the detached device was named in what it held: every block read back as"
echo "        written once it was back ($(v fixed b_new) + $(v fixed c_new)); left out"
echo "        unnamed, $c_old read back as before the write, with no error (control);"
echo "        its older block's listing taken at its word, $i_eio refused (inflight)"
