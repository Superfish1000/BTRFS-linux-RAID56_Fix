#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does a device that fails a flush after full stripes were written
# copy-on-write -- writes the write-intent log does not record -- get named in
# them, or is the commit that would reference them refused?
#
#   fullstripe_flush.sh <kernel>
#
# See fullstripe_flush_prep and fullstripe_flush_verify in init-final3.sh.
# RAID5 over three devices, RAID1 metadata, nodatasum.  Device 1 throws away
# every write (dm-flakey drop_writes) while a new file of 'B' is written with
# O_DIRECT, in full stripes; then it fails writes and flushes, and a sync's
# transaction commit has its barrier fail on it.  The file is read back cold,
# power is cut, and it is read back again on the plain disks.
#   named          16 MiB (four regions of the log): the failed flush names
#                  device 1's column or parity in every full stripe, the sync
#                  succeeds, and every block reads back as 'B', before and
#                  after the crash
#   named-control  the same with raid56_wf_full_stripe_unnamed=1: nothing is
#                  named, and the blocks of device 1's column read back as the
#                  zeros on its platter, with no error
#   refuse         360 MiB (90 regions, more than a log block that names
#                  anything describes): the commit fails and the filesystem
#                  goes read-only with the full_stripe_flush_unnamed alert;
#                  after the crash the file was never committed, and nothing
#                  reads back wrong
#   refuse-control the same with the knob: the commit goes on, and zeros read
#                  back, with no error
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: fullstripe_flush.sh <kernel>}
NDEV=3
FAIL=1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-ff.sh.$$ && mv -f $T/umltest/init-ff.sh.$$ $T/umltest/init-ff.sh	# atomic: a guest may be reading it
ulimit -c 0
arm() {	# name control MiB
	local tag=ff-$1 control=$2 mb=$3
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/ffp.$tag $T/umltest/ffv.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot() {	# mode
		local ubds=""
		for i in $(seq 0 $((NDEV-1))); do ubds="$ubds ubd$i=$D/disk$i.img"; done
		timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-ff.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$1 OPTS=rw PROFILE=raid5:raid1 TAG=$tag \
			NDEV=$NDEV FAIL=$FAIL CONTROL=$control FF_MB=$mb MNTDEV=/dev/ubda \
			< /dev/null > $D/log.$1 2>&1
		echo "boot $1 rc=$?" >> $D/log.$1
	}
	boot fullstripe_flush_prep
	boot fullstripe_flush_verify
	rm -f $D/disk*.img
}
ARMS="named named-control refuse refuse-control"
arm named 0 16
arm named-control 1 16
arm refuse 0 360
arm refuse-control 1 360
SHOW="control:|drops every|fails writes|file of|FF_|did not confirm|full stripes|aborted|readonly"
SHOW="$SHOW|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|DM_RELOAD|KNOB_FAIL"
for a in $ARMS; do
	grep -ahE "$SHOW" $T/umltest/ff-$a/log.* | cut -c1-260 | sed "s/^/  [$a] /"
done
# The value of key $3 in arm $2's report of $1 (ffp: the first boot, ffv: after
# the crash), or ?.
v() { sed -n "s/.*\<$3=\([^ ]*\).*/\1/p" $T/umltest/$1.ff-$2 2>/dev/null | head -1 | grep . || echo '?'; }
for a in $ARMS; do
	for f in ffp ffv; do
		[ -s $T/umltest/$f.ff-$a ] || {
			echo "RESULT: INCONCLUSIVE -- the $a arm did not report ($f)"; exit 2; }
	done
done
grep -lq KERNEL_SPLAT $T/umltest/ff-*/log.* && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/ff-*/log.* &&
	{ echo "RESULT: INCONCLUSIVE -- raid56_wf_full_stripe_unnamed is not there (not a CONFIG_BTRFS_DEBUG kernel?)"
	  exit 2; }
for a in $ARMS; do
	if [ "$(v ffp $a wrc)" != 0 ] || [ "$(v ffp $a commits_drop)" != 0 ]; then
		echo "RESULT: INCONCLUSIVE -- in the $a arm the file was not written without a commit (dd rc $(v ffp $a wrc), commits $(v ffp $a commits_drop))"
		exit 2
	fi
	if [ "$(v ffp $a fullw)" = 0 ] || [ "$(v ffp $a flush_errs)" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- in the $a arm no full stripe was written ($(v ffp $a fullw)) or no flush failed ($(v ffp $a flush_errs))"
		exit 2
	fi
done
# The controls must show the old defect, or the fixed arms prove nothing.
for a in named-control refuse-control; do
	if [ "$(v ffp $a sync_rc)" != 0 ] || [ "$(v ffv $a old)" = 0 ] || [ "$(v ffv $a old)" = '?' ]; then
		echo "RESULT: INCONCLUSIVE -- the $a arm did not reproduce the old behaviour (sync rc $(v ffp $a sync_rc), zeros read back after the crash $(v ffv $a old))"
		exit 2
	fi
done
for f in ffp ffv; do
	for k in old eio other; do
		[ "$(v $f named $k)" = 0 ] || {
			echo "RESULT: FAIL -- named arm ($f): $(v $f named $k) block(s) read back $k"; exit 1; }
	done
	[ "$(v $f named new)" = 256 ] || {
		echo "RESULT: FAIL -- named arm ($f): $(v $f named new) of 256 blocks read back as written"; exit 1; }
done
if [ "$(v ffp named sync_rc)" != 0 ] || [ "$(v ffp named writable_after)" != 0 ]; then
	echo "RESULT: FAIL -- named arm: the commit it could name failed (sync rc $(v ffp named sync_rc), writable $(v ffp named writable_after))"
	exit 1
fi
if [ "$(v ffp named stale_marks)" = 0 ] || [ "$(v ffp named unnamed)" != 0 ]; then
	echo "RESULT: FAIL -- named arm: stale marks $(v ffp named stale_marks), full_stripe_flush_unnamed $(v ffp named unnamed)"
	exit 1
fi
if [ "$(v ffp refuse writable_after)" = 0 ] || [ "$(v ffp refuse unnamed)" = 0 ] ||
   [ "$(v ffp refuse unnamed)" = '?' ]; then
	echo "RESULT: FAIL -- refuse arm: the commit it could not name went on (writable $(v ffp refuse writable_after), full_stripe_flush_unnamed $(v ffp refuse unnamed), sync rc $(v ffp refuse sync_rc))"
	exit 1
fi
if [ "$(v ffv refuse old)" != 0 ] || [ "$(v ffv refuse other)" != 0 ]; then
	echo "RESULT: FAIL -- refuse arm: after the crash $(v ffv refuse old) block(s) read back as zeros, $(v ffv refuse other) as something else"
	exit 1
fi
echo "RESULT: PASS -- named, every block read back as written before and after the crash ($(v ffv named new));"
echo "        with more to name than the log holds the commit failed, read-only (full_stripe_flush_unnamed"
echo "        $(v ffp refuse unnamed), sync rc $(v ffp refuse sync_rc)) and after the crash nothing read wrong (file $(v ffv refuse size));"
echo "        without the names $(v ffv named-control old) and $(v ffv refuse-control old) blocks read back as zeros, silently"
