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
#                  until the unmount the refusal keeps the names it could not
#                  write in memory (@refused_names), so on the read-only
#                  mount every block reads back as 'B', device 1's column
#                  rebuilt from the parity the others flushed; after the
#                  crash the file was never committed, and nothing reads
#                  back wrong
#   refuse-unnamedctl the same with raid56_wf_refusal_leaves_unnamed=1: the
#                  commit fails as in refuse, but nothing keeps the names,
#                  and on the read-only mount the blocks of device 1's
#                  column read back as zeros, with no error -- data whose
#                  O_DIRECT write returned, though no commit made it durable
#   refuse-control the same with raid56_wf_full_stripe_unnamed=1: the commit
#                  goes on, and zeros read back, with no error
#   inflight       384 KiB (three full stripes) in one go, each write held once
#                  its bios have completed (raid56_write_hold_ms); device 1 then
#                  fails the barrier of an fsync's commit (notreelog) while they
#                  are held: the failed flush names device 1's member in each,
#                  in flight, and the name outlives the write's completion
#                  (@hold) -- every block reads back as 'B', before and after
#                  the crash
#   inflight-control the same with raid56_wf_full_stripe_clears_hold=1: the
#                  completion clears the names, and the blocks of device 1's
#                  column read back as zeros, with no error
#
# The second argument picks the arms: all (the default), base (the first
# five) or inflight (the last two).
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: fullstripe_flush.sh <kernel> [all|base|inflight]}
WHICH=${2:-all}
case $WHICH in all|base|inflight) ;; *) echo "unknown arms $WHICH"; exit 2;; esac
NDEV=3
FAIL=1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-ff.sh.$$ && mv -f $T/umltest/init-ff.sh.$$ $T/umltest/init-ff.sh	# atomic: a guest may be reading it
ulimit -c 0
arm() {	# name control MiB [KiB held in flight]
	local tag=ff-$1 control=$2 mb=$3 kb=${4:-0}
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
			FF_INFLIGHT=$([ $kb = 0 ] && echo 0 || echo 1) FF_KB=$kb \
			< /dev/null > $D/log.$1 2>&1
		echo "boot $1 rc=$?" >> $D/log.$1
	}
	boot fullstripe_flush_prep
	boot fullstripe_flush_verify
	rm -f $D/disk*.img
}
ARMS=
if [ $WHICH != inflight ]; then
	ARMS="named named-control refuse refuse-unnamedctl refuse-control"
	arm named 0 16
	arm named-control 1 16
	arm refuse 0 360
	arm refuse-unnamedctl 3 360
	arm refuse-control 1 360
fi
if [ $WHICH != base ]; then
	ARMS="$ARMS inflight inflight-control"
	arm inflight 0 0 384
	arm inflight-control 2 0 384
fi
SHOW="control:|drops every|fails writes|file of|FF_|did not confirm|full stripes|aborted|readonly|holding"
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
for a in $ARMS; do
	grep -lq KERNEL_SPLAT $T/umltest/ff-$a/log.* && { echo "RESULT: FAIL -- kernel splat ($a)"; exit 1; }
done
for a in $ARMS; do
	grep -lq "CONTROL_KNOB_FAIL\|HOLD_KNOB_FAIL" $T/umltest/ff-$a/log.* &&
		{ echo "RESULT: INCONCLUSIVE -- a knob of the $a arm is not there (not a CONFIG_BTRFS_DEBUG kernel?)"
		  exit 2; }
done
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
if [ $WHICH != base ]; then
	for a in inflight inflight-control; do
		if [ "$(v ffp $a held)" = 0 ] || [ "$(v ffp $a held)" = '?' ] ||
		   [ "$(v ffp $a fsync_rc)" != 0 ] || [ "$(v ffp $a named_fly)" = 0 ] ||
		   [ "$(v ffp $a named_fly)" = '?' ]; then
			echo "RESULT: INCONCLUSIVE -- in the $a arm no write was held ($(v ffp $a held)), or the fsync's commit failed ($(v ffp $a fsync_rc)), or its failed flush named nothing in flight ($(v ffp $a named_fly))"
			exit 2
		fi
	done
	if [ "$(v ffp inflight-control named_landed)" != 0 ] || [ "$(v ffv inflight-control old)" = 0 ] ||
	   [ "$(v ffv inflight-control old)" = '?' ]; then
		echo "RESULT: INCONCLUSIVE -- the inflight-control arm did not reproduce the old behaviour (names once the writes landed $(v ffp inflight-control named_landed), zeros read back after the crash $(v ffv inflight-control old))"
		exit 2
	fi
	for f in ffp ffv; do
		for k in old eio other; do
			[ "$(v $f inflight $k)" = 0 ] || {
				echo "RESULT: FAIL -- inflight arm ($f): $(v $f inflight $k) block(s) read back $k"; exit 1; }
		done
		[ "$(v $f inflight new)" = 6 ] || {
			echo "RESULT: FAIL -- inflight arm ($f): $(v $f inflight new) of 6 blocks read back as written"; exit 1; }
	done
	if [ "$(v ffp inflight named_landed)" != "$(v ffp inflight named_fly)" ]; then
		echo "RESULT: FAIL -- inflight arm: the writes' completion cleared names ($(v ffp inflight named_fly) in flight, $(v ffp inflight named_landed) once they landed)"
		exit 1
	fi
	[ $WHICH = inflight ] && {
		echo "RESULT: PASS -- named while in flight, the names outlived the writes' completion ($(v ffp inflight named_landed)) and every"
		echo "        block read back as written before and after the crash ($(v ffv inflight new)); cleared by the completion"
		echo "        (control), $(v ffv inflight-control old) block(s) read back as zeros, silently"
		exit 0
	}
fi
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
# Before the crash, on the read-only mount: the file's O_DIRECT writes
# returned, and the kernel still serves it.  Keeping no names for the reads
# (refuse-unnamedctl), device 1's column reads back as the zeros on its
# platter; kept (@refused_names), every block is rebuilt from the parity
# the others flushed -- a refusal (EIO) would be for nothing.
u=refuse-unnamedctl
if [ "$(v ffp $u writable_after)" = 0 ] || [ "$(v ffp $u sync_rc)" = 0 ] ||
   [ "$(v ffp $u unnamed)" = 0 ] || [ "$(v ffp $u unnamed)" = '?' ] ||
   [ "$(v ffp $u old)" = 0 ] || [ "$(v ffp $u old)" = '?' ]; then
	echo "RESULT: INCONCLUSIVE -- the $u arm did not refuse the commit and read zeros back on the read-only"
	echo "        mount (writable $(v ffp $u writable_after), sync rc $(v ffp $u sync_rc), full_stripe_flush_unnamed"
	echo "        $(v ffp $u unnamed), zeros $(v ffp $u old)), so the refuse arm's reads there prove nothing"
	exit 2
fi
nblk=$(( $(v ffp refuse acked) / 65536 ))
echo "  on the read-only mount before the crash, new/zeros/eio/other of $nblk: refuse" \
     "$(v ffp refuse new)/$(v ffp refuse old)/$(v ffp refuse eio)/$(v ffp refuse other)," \
     "$u $(v ffp $u new)/$(v ffp $u old)/$(v ffp $u eio)/$(v ffp $u other)"
for k in old other eio; do
	[ "$(v ffp refuse $k)" = 0 ] || {
		echo "RESULT: FAIL -- refuse arm: on the read-only mount $(v ffp refuse $k) block(s) read back $k"
		exit 1; }
done
[ "$nblk" -gt 0 ] && [ "$(v ffp refuse new)" = "$nblk" ] || {
	echo "RESULT: FAIL -- refuse arm: on the read-only mount $(v ffp refuse new) of $nblk blocks read back as written"
	exit 1; }
echo "RESULT: PASS -- named, every block read back as written before and after the crash ($(v ffv named new));"
echo "        with more to name than the log holds the commit failed, read-only (full_stripe_flush_unnamed"
echo "        $(v ffp refuse unnamed), sync rc $(v ffp refuse sync_rc)); until the unmount every block read back"
echo "        as written ($(v ffp refuse new) of $nblk), and after the crash nothing read wrong (file $(v ffv refuse size));"
echo "        keeping no names for those reads, $(v ffp $u old) read back as zeros, silently ($u);"
echo "        without the names $(v ffv named-control old) and $(v ffv refuse-control old) blocks read back as zeros, silently"
[ $WHICH = all ] &&
	echo "        named in flight, the names outlived the writes' completion; cleared by it, $(v ffv inflight-control old) read back as zeros"
