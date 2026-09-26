#!/bin/bash
# Does the write-intent log say which device may have lost a write when that
# device fails a cache flush?
#
#   readd_flush.sh <kernel> [readd|disable]
#
# See readd_flush_prep and readd_flush_verify in init-final3.sh.  RAID5 data,
# RAID1 metadata, four devices.  A nodatacow file is overwritten in place, one
# 4 KiB block per full stripe, with O_DIRECT while device 1 takes the writes
# and throws them away (dm-flakey drop_writes: a volatile cache that is about
# to be lost).  Then that device fails writes and flushes (error_writes), and
# the transaction commit's barrier fails on it: the log keeps the stripes it
# was about to drop.  Power off without unmounting, boot again on the plain
# disks, read every overwritten block.
#   fixed    the kept stripes name the device's column or parity: every
#            acknowledged block reads back as written.  The writes finished
#            before the barrier was issued, so the block written after it
#            lists them as the error records the names make of them, not in
#            flight (wib_readd_base()): the recovery rebuilds the named
#            column from the parity the other devices flushed.  Listed in
#            flight, as with raid56_wf_finished_stay_inflight=1, each such
#            stripe reads to the mount as a write the crash may have torn,
#            and the blocks on the device fail with EIO (18 read, 6 EIO) --
#            refused, not wrong, but refused for nothing: that fails here
#   control  raid56_wf_no_readd_name=1 keeps them as records that do not say
#            which device: the blocks whose column is on it read back as the
#            old data, with no error
#   inflight raid56_wf_finished_stay_inflight=1, the fixed arm listing the
#            finished writes in flight as well: nothing reads back wrong, the
#            blocks on the device fail with EIO (see fixed)
#
# disable: the same, with the log being disabled (echo 0 > features/
# raid56_write_intent): its first commit, before the overwrites, writes the
# superblock without the flag, and the commit whose barrier fails is the one
# that writes the log's last block.
#   disable-fixed    that block takes the stripes back and names the device,
#                    as a failed barrier does with the log enabled: every
#                    acknowledged block reads back as written.  The flushes
#                    that write it cover writes the disabled log does not
#                    note, so it names the device's member of every stripe
#                    it keeps, written or not, and the recovery rebuilds
#                    that member from the parity: blocks nobody overwrote
#                    read back as they were.  With
#                    raid56_wf_finished_stay_inflight=1 the stripes stay
#                    listed in flight as well, the recovery records them
#                    undecidable, and the overwritten blocks on the device
#                    and those nobody overwrote read EIO (6 and 12): that
#                    fails here too
#   disable-inflight raid56_wf_finished_stay_inflight=1: as disable-fixed,
#                    but those blocks fail with EIO (see disable-fixed)
#   disable-control  raid56_wf_disable_forgets_writes=1: the block ignores
#                    the failed barrier and lists nothing it dropped; the
#                    blocks whose column is on the device read back as the
#                    old data, with no error
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: readd_flush.sh <kernel> [readd|disable]}
PAIR=${2:-readd}
case $PAIR in
readd) A=fixed; B=control; I=inflight; CA=0; CB=1; DIS=0; KNOB=raid56_wf_no_readd_name;;
disable) A=disable-fixed; B=disable-control; I=disable-inflight; CA=0; CB=2; DIS=1
	 KNOB=raid56_wf_disable_forgets_writes;;
*) echo "unknown pair $PAIR"; exit 2;;
esac
# The third arm of either pair: the fixed kernel with the finished writes left
# listed in flight in the block written after the failed barrier.
IKNOB=raid56_wf_finished_stay_inflight
NDEV=4
FAIL=1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-rf.sh.$$ && mv -f $T/umltest/init-rf.sh.$$ $T/umltest/init-rf.sh	# atomic: a guest may be reading it
ulimit -c 0
arm() {	# name control [kernel args]
	local tag=rf-$1 control=$2 kargs="${3:-}"
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/rf.$tag $T/umltest/rf-prep.$tag $T/umltest/results.$tag \
	      $T/umltest/nocow.acked.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot() {	# mode
		local ubds=""
		for i in $(seq 0 $((NDEV-1))); do ubds="$ubds ubd$i=$D/disk$i.img"; done
		# commit=600: no transaction commit, and so no barrier, may run
		# while the device drops writes (the guest checks and says so).
		timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-rf.sh $ubds $kargs quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$1 OPTS=rw,commit=600 PROFILE=raid5:raid1 TAG=$tag \
			NDEV=$NDEV FAIL=$FAIL CONTROL=$control MNTDEV=/dev/ubda DISABLE=$DIS \
			< /dev/null > $D/log.$1 2>&1
		echo "boot $1 rc=$?" >> $D/log.$1
	}
	boot readd_flush_prep
	boot readd_flush_verify
	rm -f $D/disk*.img
}
arm $A $CA
arm $B $CB
arm $I 0 btrfs.$IKNOB=1
SHOW="control:|drops every|fails writes|in-place overwrites|RF_|RF |did not confirm|disable"
SHOW="$SHOW|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|DM_RELOAD|KNOB_FAIL"
for a in $A $B $I; do
	grep -ahE "$SHOW" $T/umltest/rf-$a/log.* | sed "s/^/  [$a] /"
done
read -r f_acked f_fl f_stale f_commit < $T/umltest/rf-prep.rf-$A 2>/dev/null || f_acked=?
read -r c_acked c_fl c_stale c_commit < $T/umltest/rf-prep.rf-$B 2>/dev/null || c_acked=?
read -r i_acked i_fl i_stale i_commit < $T/umltest/rf-prep.rf-$I 2>/dev/null || i_acked=?
read -r f_new f_old f_eio f_other f_side f_side_eio < $T/umltest/rf.rf-$A 2>/dev/null ||
	f_new=?
read -r c_new c_old c_eio c_other c_side c_side_eio < $T/umltest/rf.rf-$B 2>/dev/null ||
	c_new=?
read -r i_new i_old i_eio i_other i_side i_side_eio < $T/umltest/rf.rf-$I 2>/dev/null ||
	i_new=?
case "$f_acked$c_acked$i_acked$f_new$c_new$i_new" in *'?'*)
	echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/rf-$A/log.* $T/umltest/rf-$B/log.* $T/umltest/rf-$I/log.* &&
	{ echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/rf-$B/log.* &&
	{ echo "RESULT: INCONCLUSIVE -- $KNOB is not there (not a CONFIG_BTRFS_DEBUG kernel?)"
	  exit 2; }
if grep -lq "RF_DISABLED_EARLY\|RF_NOT_DISABLED\|DISABLE_FAIL" $T/umltest/rf-$A/log.* \
	$T/umltest/rf-$B/log.* $T/umltest/rf-$I/log.*; then
	echo "RESULT: INCONCLUSIVE -- the log was not disabled by the commit whose barrier failed"
	exit 2
fi
if [ "$f_commit" != 0 ] || [ "$c_commit" != 0 ] || [ "$i_commit" != 0 ]; then
	echo "RESULT: INCONCLUSIVE -- a transaction commit ran while the device dropped writes"; exit 2
fi
if [ "${f_fl:-0}" = 0 ] || [ "${c_fl:-0}" = 0 ] || [ "${i_fl:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- no flush failed (flush_io_errs fixed=$f_fl control=$c_fl" \
	     "inflight=$i_fl)"
	exit 2
fi
if [ "$f_acked" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the fixed arm had no overwrite acknowledged, nothing to read"
	exit 2
fi
if [ "$c_old" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control read nothing back old, so a clean fixed arm proves nothing"; exit 2
fi
[ "${f_stale:-0}" -gt 0 ] || echo "  note: the fixed arm recorded no stale marks after the failed flush"
if [ "$f_old" != 0 ] || [ "$f_other" != 0 ]; then
	echo "RESULT: FAIL -- acknowledged data read back wrong without an error: old=$f_old other=$f_other"; exit 1
fi
if [ "$f_side" != 0 ]; then
	echo "RESULT: FAIL -- $f_side blocks nobody overwrote read back wrong, without an error"
	exit 1
fi
if [ "${f_side_eio:-0}" != 0 ]; then
	echo "RESULT: FAIL -- $f_side_eio blocks nobody overwrote failed to read"; exit 1
fi
# Refused, not wrong -- but the names say exactly what the failed flush may
# have lost, and the parity the other devices flushed rebuilds it: a refusal
# here takes a stripe for possibly torn that no crash could have torn.
if [ "${f_eio:-0}" != 0 ]; then
	echo "RESULT: FAIL -- $f_eio acknowledged block(s) the names prove failed to read (EIO)"
	exit 1
fi
# The inflight arm refuses (the names are there, the listing says torn): it
# must never read anything wrong, and it must refuse something, or the fixed
# arm's clean reads do not show what the listing is for.
if [ "$i_old" != 0 ] || [ "$i_other" != 0 ] || [ "${i_side:-0}" != 0 ]; then
	echo "RESULT: FAIL -- with $IKNOB=1 blocks read back wrong without an error:"
	echo "        old=$i_old other=$i_other side=$i_side"
	exit 1
fi
if [ "$((${i_eio:-0} + ${i_side_eio:-0}))" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- with $IKNOB=1 nothing was refused either, so the fixed"
	echo "        arm's clean reads do not show the listing (not a CONFIG_BTRFS_DEBUG kernel?)"
	exit 2
fi
if [ $PAIR = disable ]; then
	echo "RESULT: PASS -- the log's last block, written by the commit whose barrier failed,"
	echo "        named the device: every acknowledged block read back as written ($f_new),"
	echo "        and so did every block nobody overwrote; forgetting what it dropped,"
	echo "        $c_old of $c_acked read back as the old data, silently; listing the"
	echo "        finished writes in flight ($IKNOB=1), $i_eio of them and $i_side_eio"
	echo "        blocks nobody overwrote failed (EIO)"
	exit 0
fi
echo "RESULT: PASS -- every acknowledged block read back as written ($f_new);"
echo "        without the names $c_old of $c_acked read back as the old data, silently;"
echo "        listing the finished writes in flight ($IKNOB=1), $i_eio failed (EIO)"
