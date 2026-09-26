#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Is a write that a failed flush may have torn still treated as one after a
# crash, when the log could not name the device in its record?
#
#   torn_readd.sh <kernel> [omit-device] [readd|upgrade|upgrade-verdict]
#
# See torn_readd_prep and torn_readd_verify in init-final3.sh.  RAID5 data,
# RAID1C3 metadata, four devices.  While device 1 drops writes, one block per
# region of a nodatacow file is overwritten in place; then device 1 fails a
# commit's barrier.  The readd takes every record back, but more regions than
# a block that names anything can describe need a name, so it takes them back
# without (log_flush_unnamed) and marks those device 1 got a write in possibly
# torn.  Device 1 is healed, a commit every device confirms drops what was in
# flight -- from then on only the mark says those writes may be torn -- and
# the machine stops without unmounting.  Then the array is mounted with
# another device (default 2) left out, read-only and then read-write, and
# every row an overwrite rewrote the parity of is read where it lies on that
# device: rebuilt from a parity the dropped write left not describing the
# data wherever device 1 held the written column or the parity.
#   fixed    the records say possibly torn (written as in flight): the
#            read-only mount refuses those rebuilds (btrfs_wib_unrecovered()),
#            the read-write one records the stripes undecidable
#            (scrub_raid56_recover_absent()): each block reads what was
#            acknowledged, or fails
#   control  raid56_wf_torn_no_persist=1: the records reach the disk as plain
#            failed writes, whose rules rebuild the column as it is: blocks
#            read back as a value nobody wrote, with no error
# The blocks of device 1's own column are not scored: where its write was
# dropped they read as the old data, which the log could not name -- that is
# what the log_flush_unnamed alert says.
#
# upgrade: the same, with the log a kernel from before the mark leaves --
# the prep runs with raid56_wf_no_readd_name=1 (the readd takes the records
# back as plain error records, naming and marking nothing) and
# raid56_wf_log_unmarked=1 (its blocks lack BTRFS_WIB_TORN_MARKING); the
# verify boots are this kernel as it is:
#   upgrade          a block without the marker cannot say which of its error
#                    records hides a torn write, so all of them are read as
#                    possibly torn: with the device left out none of the rows
#                    checked can be decided, each block fails, and no record
#                    is spent
#   upgrade-control  raid56_wf_trust_unmarked_log=1 in the verify boots: the
#                    records read as plain failed writes, and blocks read
#                    back as a value nobody wrote, with no error
#
# upgrade-verdict: the upgrade arm again, against what its read-write phase
# also shows.  Its recovery takes back more regions than a wide log block
# describes (82), and records ~70 of the stripes undecidable (a verdict:
# every present parity stale, @suspect_par).  Fixed, a verdict does not make
# the block wide, nothing is spent, and the verdicts refuse the rebuilds.
#   upgrade-verdict  raid56_wf_suspect_as_stale=1 in the verify boots: the
#                    first verdict makes the block wide, and a full log with
#                    a device missing spends records in table order, verdicts
#                    among them (sticky_evicted): the read-write phase reads
#                    their rows back as a value nobody wrote, with no error.
#                    The read-only phase, which records no verdict, refuses
#                    as the upgrade arm does.
# INCONCLUSIVE unless the log held more than 82 regions, the upgrade arm's
# recovery recorded a verdict, and the control both spent records and read
# something wrong.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: torn_readd.sh <kernel> [omit-device] [readd|upgrade|upgrade-verdict]}
OMIT=${2:-2}
PAIR=${3:-readd}
case $PAIR in
readd) A=fixed; B=control; CA=0; CB=1;;
upgrade) A=upgrade; B=upgrade-control; CA=2; CB=3;;
# Its own tag for the same fixed arm: the upgrade pair may run at the same time.
upgrade-verdict) A=upgrade-v; B=upgrade-verdict; CA=2; CB=4;;
*) echo "unknown pair $PAIR"; exit 2;;
esac
NDEV=4
FAIL=1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-tr.sh.$$ &&
	mv -f $T/umltest/init-tr.sh.$$ $T/umltest/init-tr.sh
cp $HERE/raid56_rows.py $T/umltest/raid56_rows.py.$$ &&
	mv -f $T/umltest/raid56_rows.py.$$ $T/umltest/raid56_rows.py
ulimit -c 0
[ "$FAIL" = "$OMIT" ] && { echo "fail and omit devices must differ"; exit 2; }
arm() {	# name control
	local tag=tr-$1 control=$2
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/tr-prep.$tag $T/umltest/tr.$tag.* $T/umltest/tr.rows.$tag \
	      $T/umltest/tr.acked.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot() {	# mode phase omit opts
		local ubds="" mnt="" d
		for d in $(seq 0 $((NDEV-1))); do
			[ "$d" = "$3" ] && continue
			ubds="$ubds ubd$d=$D/disk$d.img"
			[ -n "$mnt" ] || mnt=/dev/ubd$(echo abcdefgh | cut -c$((d + 1)))
		done
		timeout 1800 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-tr.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$1 OPTS=$4 PROFILE=raid5:raid1c3 TAG=$tag \
			NDEV=$NDEV FAIL=$FAIL CONTROL=$control MNTDEV=$mnt \
			OMITTED=$([ "$3" = none ] || echo "$3") PHASE=$2 \
			< /dev/null > $D/log.$2 2>&1
		echo "boot $1 $2 rc=$?" >> $D/log.boots
	}
	# commit=600: no transaction commit, and so no barrier, may run while
	# the device drops writes (the guest checks and says so).
	boot torn_readd_prep prep none rw,commit=600
	boot torn_readd_verify ro $OMIT ro
	boot torn_readd_verify rw $OMIT rw
	rm -f $D/disk*.img
}
arm $A $CA
arm $B $CB
SHOW="control:|drops every|fails writes|in-place overwrites|TR_|TR |did not confirm|too full"
SHOW="$SHOW|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|LAYOUT_FAIL|DM_RELOAD|KNOB_FAIL"
SHOW="$SHOW|upgrade:|does not mark"
for a in $A $B; do
	grep -ahE "$SHOW" $T/umltest/tr-$a/log.* | grep -v "TR_WRONG\|TR_DIRECT_BAD" |
		sed "s/^/  [$a] /"
	echo "  [$a] TR_WRONG lines (prep ro rw):" \
	     $(for p in prep ro rw; do grep -ac "TR_WRONG" $T/umltest/tr-$a/log.$p 2>/dev/null ||
		  true; done)
done
prep() { cat $T/umltest/tr-prep.tr-$1 2>/dev/null || echo "? ? ? ? ? ? ?"; }
# checked ok eio wrong torn_rows fail_old direct_bad recovery_suspect
# sticky_evicted regions (pending_recovery_regions: what the log listed at
# mount, before a read-write recovery takes it over)
res() { cat $T/umltest/tr.tr-$1.$2 2>/dev/null || echo "? ? ? ? ? ? ? ? ? ?"; }
read -r f_acked f_fl f_unn f_torn1 f_torn2 f_commit f_sticky <<<"$(prep $A)"
read -r c_acked c_fl c_unn c_torn1 c_torn2 c_commit c_sticky <<<"$(prep $B)"
read -r fr_chk fr_ok fr_eio fr_wrong fr_torn fr_fold fr_dbad _ _ fr_regs <<<"$(res $A ro)"
read -r fw_chk fw_ok fw_eio fw_wrong fw_torn fw_fold fw_dbad fw_sus fw_ev _ <<<"$(res $A rw)"
read -r cr_chk cr_ok cr_eio cr_wrong cr_torn cr_fold cr_dbad _ _ cr_regs <<<"$(res $B ro)"
read -r cw_chk cw_ok cw_eio cw_wrong cw_torn cw_fold cw_dbad cw_sus cw_ev _ <<<"$(res $B rw)"
echo "  prep: acknowledged $f_acked/$c_acked, flush errors $f_fl/$c_fl," \
     "log_flush_unnamed $f_unn/$c_unn, torn_blocks after the readd $f_torn1/$c_torn1," \
     "after the drop $f_torn2/$c_torn2, sticky_blocks $f_sticky/$c_sticky ($A/$B)"
echo "  device $OMIT's blocks of the rewritten rows, ok/eio/wrong of checked (rows torn):"
echo "    $A ro $fr_ok/$fr_eio/$fr_wrong of $fr_chk ($fr_torn)," \
     "rw $fw_ok/$fw_eio/$fw_wrong of $fw_chk ($fw_torn)"
echo "    $B ro $cr_ok/$cr_eio/$cr_wrong of $cr_chk ($cr_torn)," \
     "rw $cw_ok/$cw_eio/$cw_wrong of $cw_chk ($cw_torn)"
echo "  device $FAIL's dropped blocks read old (not scored): $A $fr_fold/$fw_fold," \
     "$B $cr_fold/$cw_fold; other devices' blocks read wrong: $A $fr_dbad/$fw_dbad"
echo "  regions the log listed at mount: $A ${fr_regs:-?}, $B ${cr_regs:-?};" \
     "read-write recovery_suspect $A ${fw_sus:-?}, $B ${cw_sus:-?};" \
     "sticky_evicted $A ${fw_ev:-?}, $B ${cw_ev:-?}"
case "$f_acked$c_acked$fr_chk$fw_chk$cr_chk$cw_chk" in
*'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;;
esac
grep -lq KERNEL_SPLAT $T/umltest/tr-*/log.* && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/tr-$A/log.* $T/umltest/tr-$B/log.* && {
	echo "RESULT: INCONCLUSIVE -- a raid56_wf_* knob is not there"
	echo "        (not a CONFIG_BTRFS_DEBUG kernel?)"; exit 2
}
if [ "$f_commit" != 0 ] || [ "$c_commit" != 0 ]; then
	echo "RESULT: INCONCLUSIVE -- a transaction commit ran while the device dropped writes"
	exit 2
fi
if [ "${f_fl:-0}" = 0 ] || [ "${c_fl:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- no flush failed (flush_io_errs fixed=$f_fl control=$c_fl)"
	exit 2
fi
if [ $PAIR = readd ] && { [ "${f_unn:-0}" = 0 ] || [ "${f_torn1:-0}" = 0 ]; }; then
	echo "RESULT: INCONCLUSIVE -- the readd named everything or marked nothing"
	echo "        (log_flush_unnamed $f_unn, torn_blocks $f_torn1):" \
	     "no record here needed the mark"
	exit 2
fi
if [ $PAIR != readd ]; then
	if [ "${f_torn1:-0}" != 0 ] || [ "${f_sticky:-0}" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the prep did not leave the log an older kernel"
		echo "        leaves (torn_blocks $f_torn1, sticky_blocks $f_sticky)"
		exit 2
	fi
	grep -aq "does not mark the writes" $T/umltest/tr-$A/log.ro || {
		echo "RESULT: FAIL -- the verify mount did not take the log for an unmarked one"
		exit 1
	}
fi
if [ "$fr_wrong" != 0 ] || [ "$fw_wrong" != 0 ]; then
	echo "RESULT: FAIL -- a rebuild from a torn parity was read as data with the mark:" \
	     "ro $fr_wrong, rw $fw_wrong"
	exit 1
fi
if [ "$fr_dbad" != 0 ] || [ "$fw_dbad" != 0 ]; then
	echo "RESULT: FAIL -- blocks on the other devices read back wrong: ro $fr_dbad, rw $fw_dbad"
	exit 1
fi
# The upgrade arm: with the device left out, no row checked can be decided --
# every error record is possibly torn -- so a row that reads ok is a rebuild
# from a possibly torn parity that happened to be right, and nothing in the
# read-write phase writes, so nothing there may spend a record.
if [ $PAIR != readd ] && { [ "$fr_ok" != 0 ] || [ "$fw_ok" != 0 ] || [ "$fw_ev" != 0 ]; }; then
	echo "RESULT: FAIL -- the fixed arm spent $fw_ev record(s) and handed back rebuilds of"
	echo "        undecided stripes: ro $fr_ok, rw $fw_ok"
	exit 1
fi
if [ $PAIR = readd ] && [ "${f_torn2:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- something retired the marks before the crash"
	echo "        (torn_blocks $f_torn1 after the readd, 0 after the drop)"
	exit 2
fi
if [ $PAIR = upgrade-verdict ]; then
	case "${fr_regs:-?}${fw_sus:-?}${cw_ev:-?}" in
	*'?'*) echo "RESULT: INCONCLUSIVE -- a verify boot did not report its counters"; exit 2;;
	esac
	if [ "$fr_regs" -le 82 ]; then
		echo "RESULT: INCONCLUSIVE -- the log listed $fr_regs regions, no more than a wide"
		echo "        block describes (82): the control had nothing to spend"
		exit 2
	fi
	if [ "$fw_sus" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the upgrade arm's recovery recorded no stripe"
		echo "        undecidable: no verdict for the control to spend"
		exit 2
	fi
	# The read-only phase records no verdict: the knob changes nothing there.
	if [ "$cr_wrong" != 0 ] || [ "$cr_dbad" != 0 ]; then
		echo "RESULT: FAIL -- the control's read-only phase, which the knob does not" \
		     "touch, read wrong: missing column $cr_wrong, other devices $cr_dbad"
		exit 1
	fi
	if [ "$cw_ev" = 0 ] || [ "$cw_wrong" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the control spent no record or read nothing wrong"
		echo "        (sticky_evicted $cw_ev, wrong $cw_wrong), so a clean fixed arm"
		echo "        proves nothing"
		exit 2
	fi
	echo "RESULT: PASS -- with verdicts on $fw_sus undecided stripes in a log of $fr_regs"
	echo "        regions, no record was spent and every rebuild they cover was"
	echo "        refused (rw eio $fw_eio); a verdict counted as a stale parity made the log"
	echo "        wide, $cw_ev records were spent, and $cw_wrong blocks read back as a value"
	echo "        nobody wrote, with no error"
	exit 0
fi
if [ "$cr_wrong" = 0 ] && [ "$cw_wrong" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control read nothing wrong either, so a clean"
	echo "        fixed arm proves nothing"
	exit 2
fi
if [ $PAIR = upgrade ]; then
	echo "RESULT: PASS -- after the crash the error records of a log without the marker"
	echo "        refused every rebuild of the missing column they could not vouch for"
	echo "        (ro eio $fr_eio, rw eio $fw_eio); trusted as plain failed writes,"
	echo "        $cr_wrong (ro) and $cw_wrong (rw) blocks read back as a value nobody wrote"
	exit 0
fi
echo "RESULT: PASS -- after the crash the possibly torn records refused every rebuild"
echo "        of the missing column they could not vouch for (ro eio $fr_eio, rw eio $fw_eio);"
echo "        written as plain failed writes, $cr_wrong (ro) and $cw_wrong (rw) blocks read"
echo "        back as a value nobody wrote, with no error"
