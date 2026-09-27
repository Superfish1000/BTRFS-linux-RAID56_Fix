#!/bin/bash
# Does a full write-intent log with a device missing refuse, rather than spend
# the records that name stale data?
#
#   degraded_log_full.sh <kernel> [column|unrelated]
#
# See degraded_log_full in init-final3.sh.
# column: RAID5 data and metadata, a device removed, mounted degraded, then
# small writes into more 4 MiB regions than the write-intent log holds.  Every
# one of them leaves a record naming the missing device, which nothing can
# retire while it is gone.
#   fixed    the log keeps them: writes into new regions fail with the log_full
#            alert, which says to replace the missing devid with a new disk and
#            not to reconnect the old one; metadata may go read-only; every
#            acknowledged block reads back as written, and no record is dropped
#   control  raid56_wf_evict_stage0=1: the log spends them (record_dropped),
#            every write succeeds and metadata commits, as stage 0 did
#   And the fixed arm spends no record at all (record_dropped 0): on four
#   devices a full stripe can run from one region into the next, and the
#   first write goes into one such stripe past the others, whose record is
#   two entries, one naming the missing member and one nothing; the half
#   that names nothing is not spent while the other names the member --
#   the straddle arm (raid56_wf_spend_straddling_half=1, column only)
#   spends it with a record_dropped nothing warranted.
# unrelated (CUR-6): RAID5 data and RAID1 metadata on three devices, a fourth
# added that holds none of it, then removed: degraded, though nothing of the
# RAID5 chunk is missing.  Device 1 fails the writes of 82 regions (one block
# each, in its data column), its repairs give up, it is healed, a commit
# acknowledges them; then 20 writes into new regions, elsewhere.
#   fixed    the log keeps the records naming device 1: the new writes fail
#            (log_full), every block of the first writes reads back as written,
#            and once a scrub has repaired them a new write succeeds
#   control  raid56_wf_evict_stage0=1: the new writes spend records naming
#            device 1, and their blocks read back from it as they were before
#            the write, with no error
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: degraded_log_full.sh <kernel> [column|unrelated]}
PLAN=${2:-column}
case $PLAN in column|unrelated) ;; *) echo "unknown plan $PLAN"; exit 2;; esac
NDEV=4
OPTS=rw
[ $PLAN = unrelated ] && OPTS=rw,commit=600
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-dlf.sh.$$ && mv -f $T/umltest/init-dlf.sh.$$ $T/umltest/init-dlf.sh	# atomic: a guest may be reading it
cp $HERE/raid56_rows.py $T/umltest/raid56_rows.py.$$ &&
	mv -f $T/umltest/raid56_rows.py.$$ $T/umltest/raid56_rows.py
ulimit -c 0
arm() {	# name control
	local tag=dlf-$PLAN-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/dlf.$tag $T/umltest/dlf.*.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-dlf.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=degraded_log_full OPTS=$OPTS PROFILE=raid5:raid5 TAG=$tag \
		NDEV=$NDEV CONTROL=$2 PLAN=$PLAN NREG=${NREG:-120} < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
ARMS="fixed control"
arm fixed 0
arm control 1
[ $PLAN = column ] && { ARMS="$ARMS straddle"; arm straddle 2; }
for a in $ARMS; do
	grep -ah "mounted degraded\|writes ok\|writes into new\|DLF \|control:\|healed\|fails writes\|scrub rc\|log is full of records\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|LAYOUT_FAIL\|KNOB_FAIL\|DEVICE_ADD_FAIL" \
		$T/umltest/dlf-$PLAN-$a/log | grep -v DLF_READ_BAD | cut -c1-400 | sed "s/^/  [$a] /"
	echo "  [$a] DLF_READ_BAD lines: $(grep -ac DLF_READ_BAD $T/umltest/dlf-$PLAN-$a/log)"
done
res() { cat $T/umltest/dlf.dlf-$PLAN-$1 2>/dev/null || echo "?"; }
read -r f_ok f_eio f_meta f_ro f_drop f_full f_acked f_rok f_reio f_rbad f_fresh f_sev f_stev \
	<<<"$(res fixed)"
read -r c_ok c_eio c_meta c_ro c_drop c_full c_acked c_rok c_reio c_rbad c_fresh c_sev c_stev \
	<<<"$(res control)"
s_ok=- s_drop=- s_stev=- s_sev=- s_rbad=-
[ $PLAN = column ] &&
	read -r s_ok s_eio s_meta s_ro s_drop s_full s_acked s_rok s_reio s_rbad s_fresh s_sev s_stev \
		<<<"$(res straddle)"
case "$f_ok$c_ok$s_ok" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/dlf-$PLAN-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq WATCHDOG $T/umltest/dlf-$PLAN-*/log && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
for a in $ARMS; do
	grep -lq CONTROL_KNOB_FAIL $T/umltest/dlf-$PLAN-$a/log &&
		{ echo "RESULT: INCONCLUSIVE -- the $a knob is not there (not a CONFIG_BTRFS_DEBUG kernel?)"; exit 2; }
done
echo "  new writes ok/eio: fixed $f_ok/$f_eio, control $c_ok/$c_eio; metadata committed fixed $f_meta control $c_meta," \
     "read-only fixed $f_ro control $c_ro"
echo "  log_full fixed $f_full control $c_full, record_dropped fixed $f_drop control $c_drop," \
     "records naming a member spent (stale_evicted) fixed $f_sev control $c_sev"
echo "  read back ok/eio/wrong of acknowledged: fixed $f_rok/$f_reio/$f_rbad of $f_acked," \
     "control $c_rok/$c_reio/$c_rbad of $c_acked"
if [ $PLAN = column ]; then
	if [ "$c_eio" != 0 ] || [ "$c_meta" != 1 ] || [ "${c_drop:-0}" -lt 1 ]; then
		echo "RESULT: INCONCLUSIVE -- the control did not spend records and keep writing"
		echo "        (eio=$c_eio meta=$c_meta record_dropped=$c_drop), so it did not reproduce stage 0"
		exit 2
	fi
	[ "${f_acked:-0}" -gt 0 ] || { echo "RESULT: INCONCLUSIVE -- no write was acknowledged"; exit 2; }
	if [ "${f_sev:-0}" != 0 ]; then
		echo "RESULT: FAIL -- $f_sev record(s) naming a member were spent (record_dropped $f_drop)"; exit 1
	fi
	# Nor the half of a stripe straddling two regions that names nothing,
	# its other half naming the missing column: a record_dropped nothing
	# warranted.  The straddle arm spends it (or finds none to spend).
	echo "  straddle (raid56_wf_spend_straddling_half=1): records spent $s_stev, record_dropped $s_drop," \
	     "naming a member $s_sev, read back wrong $s_rbad; fixed: records spent $f_stev"
	if [ "${s_stev:-0}" = 0 ] || [ "${s_sev:-0}" != 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the straddle arm spent no unnamed half ($s_stev record(s), $s_sev naming),"
		echo "        so a fixed arm that spends none proves nothing about it"
		exit 2
	fi
	if [ "${f_drop:-0}" != 0 ] || [ "${f_stev:-0}" != 0 ]; then
		echo "RESULT: FAIL -- the fixed arm spent $f_stev record(s) (record_dropped $f_drop)"; exit 1
	fi
	if [ "$f_eio" = 0 ] || [ "${f_full:-0}" -lt 1 ]; then
		echo "RESULT: FAIL -- the full log did not refuse (eio=$f_eio log_full=$f_full)"; exit 1
	fi
	if [ "${f_rbad:-0}" != 0 ]; then
		echo "RESULT: FAIL -- $f_rbad acknowledged block(s) read back wrong"; exit 1
	fi
	# Every acknowledged block was read: none left out of the count.
	if [ $(( ${f_rok:-0} + ${f_reio:-0} + ${f_rbad:-0} )) != "$f_acked" ]; then
		echo "RESULT: FAIL -- $f_rok ok, $f_reio EIO and $f_rbad wrong of $f_acked acknowledged blocks:"
		echo "        not every one was read back"
		exit 1
	fi
	grep -aq "replace that devid with a NEW disk the same way, and do not reconnect the old one" \
		$T/umltest/dlf-$PLAN-fixed/log ||
		{ echo "RESULT: FAIL -- the log_full explanation does not say to replace the missing devid"; exit 1; }
	echo "RESULT: PASS -- degraded, the full log refused $f_eio write(s) with the log_full alert and spent"
	echo "        no record naming a member, no acknowledged block reads back wrong ($f_rok ok, $f_reio EIO"
	echo "        of $f_acked);"
	echo "        control (stage 0) spent records: record_dropped $c_drop, every write succeeded;"
	echo "        spending the unnamed half of a named stripe (straddle) raised record_dropped $s_drop"
	exit 0
fi
# unrelated
if [ "${c_drop:-0}" -lt 1 ] || [ "${c_rbad:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control spent no record naming device 1 or read nothing wrong"
	echo "        (record_dropped $c_drop, wrong $c_rbad), so a clean fixed arm proves nothing"
	exit 2
fi
[ "${f_acked:-0}" -gt 0 ] || { echo "RESULT: INCONCLUSIVE -- no write was acknowledged"; exit 2; }
if [ "${f_drop:-0}" != 0 ] || [ "${f_sev:-0}" != 0 ] || [ "${f_rbad:-0}" != 0 ]; then
	echo "RESULT: FAIL -- record_dropped $f_drop (naming a member $f_sev), $f_rbad acknowledged block(s)"
	echo "        read back wrong"
	exit 1
fi
if [ "$f_eio" = 0 ] || [ "${f_full:-0}" -lt 1 ]; then
	echo "RESULT: FAIL -- the full log did not refuse (eio=$f_eio log_full=$f_full)"; exit 1
fi
if [ "${f_rok:-0}" != "$f_acked" ]; then
	echo "RESULT: FAIL -- $f_rok of $f_acked blocks device 1 failed read back as written"
	echo "        ($f_reio EIO): a device that is not missing holds their parity"
	exit 1
fi
if [ "$f_fresh" != ok ]; then
	echo "RESULT: FAIL -- after the scrub the alert asks for, a write into a new region still failed"; exit 1
fi
echo "RESULT: PASS -- with an unrelated device missing, the full log refused $f_eio write(s) (log_full) and"
echo "        every block device 1 failed reads back as written ($f_rok of $f_acked); after a scrub a new"
echo "        write succeeded.  Control (stage 0): record_dropped $c_drop, $c_rbad block(s) read back old"
echo "        with no error (CUR-6)"
