#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Do the records a failed flush leaves wedge the write-intent log once it is
# full of them, with every device there?
#
#   flush_wedge.sh <kernel> [named|torn|busy|unnamed]
#
# See flush_wedge in init-final3.sh.  RAID5 data, RAID1 metadata, four
# devices.  While device 1 drops writes, one block per region is overwritten
# where device 1 holds its data column or parity; then device 1 fails a
# commit's barrier and is healed, and FW_FRESH (20) writes go into new
# regions, one at a time.  Then every overwritten block is read back.
#   named  82 regions: the readd names device 1 in every one, which fills a
#          wide block
#     fixed    it asks for their repair and says so (device_write_failed);
#              the repairs retire the records and every new write succeeds;
#              every overwritten block reads back as written or fails
#     control  raid56_wf_readd_no_repair=1: nothing retires them, and the
#              new writes fail at once (log_full)
#   The named control's log_full explanation must name scrub as the remedy.
#   torn   165 regions: too many to name, the readd marks them possibly torn,
#          which fills a narrow block
#     fixed    a full log keeps them: the new writes fail at once (log_full),
#              its explanation says a scrub retires them, and after one a
#              write into a new region succeeds; nothing is dropped
#     control  raid56_wf_evict_stage0=1: it spends them, last and with the
#              alert (record_dropped), and every new write succeeds
#   busy   164 regions, one slot left, and instead of FW_FRESH writes one at a
#          time, FW_BUSY (8) at once into new regions whose parity device 1
#          holds, started while device 1 is suspended: they queue behind it
#          and go together once it is back, the first to be recorded takes the
#          free slot, and the others find the log full with a write in flight
#          that frees a slot when it finishes
#     fixed    they wait for it and take its slot in turn: no possibly torn
#              record is spent, every write succeeds
#     control  raid56_wf_evict_stage0=1 and raid56_wf_torn_spent_eagerly=1:
#              each spends one at once (sticky_evicted, record_dropped)
#   torn and busy set raid56_wf_readd_acks_unnamed=1 in both arms: the
#   default refuses the commit that would leave such a log (unnamed).  busy
#   sets raid56_wf_admit_narrow=1 in both arms too: the default admits no
#   write into a log holding more of those records than a wide block
#   describes, and refuses all eight (commit_full.sh).
#   unnamed (CUR-4)  165 regions, as torn: too many to name device 1 in
#     fixed    the commit whose barrier failed fails instead (read-only), with
#              the log_flush_unnamed alert: the overwrites are never
#              acknowledged -- and until the unmount the refused readd keeps
#              in memory the names it could not write (@refused_names), so
#              every block reads back as written: device 1's dropped column
#              is rebuilt from the parity the others flushed
#     control  raid56_wf_readd_acks_unnamed=1: the commit goes on, and the
#              overwrites device 1 dropped read back as the block was before
#              the acknowledged write, with no error
#     unnamedctl raid56_wf_refusal_leaves_unnamed=1: the commit fails as in
#              fixed, but nothing keeps the names, and until the unmount the
#              overwrites device 1 dropped read back as before, with no error
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: flush_wedge.sh <kernel> [named|torn|busy|unnamed]}
PLAN=${2:-named}
case $PLAN in named|torn|busy|unnamed) ;; *) echo "unknown plan $PLAN"; exit 2;; esac
OPTS=rw,commit=600
# More RMW workers than the three one CPU gets, or no more than three writes
# are ever recorded at once (rmw_workers).
[ $PLAN = busy ] && OPTS=$OPTS,thread_pool=16
NDEV=4
FAIL=1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-fw.sh.$$ && mv -f $T/umltest/init-fw.sh.$$ $T/umltest/init-fw.sh
cp $HERE/raid56_rows.py $T/umltest/raid56_rows.py.$$ &&
	mv -f $T/umltest/raid56_rows.py.$$ $T/umltest/raid56_rows.py
ulimit -c 0
arm() {	# name control
	local tag=fw-$PLAN-$1 ubds=""
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/fw.$tag $T/umltest/fw.*.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do
		truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"
	done
	# commit=600: no transaction commit, and so no barrier, may run while
	# the device drops writes (the guest checks and says so).
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-fw.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=flush_wedge OPTS=$OPTS PROFILE=raid5:raid1 \
		TAG=$tag NDEV=$NDEV FAIL=$FAIL PLAN=$PLAN CONTROL=$2 < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
ARMS="fixed control"
[ $PLAN = unnamed ] && { arm unnamedctl 2; ARMS="$ARMS unnamedctl"; }
for a in $ARMS; do
	grep -ah "control:\|FW \|FW_\|drops every\|fails writes\|healed\|in-place overwrites\|suspended\|resumed\|KERNEL_SPLAT\|WATCHDOG\|MOUNT_FAIL\|MKFS_FAIL\|LAYOUT_FAIL\|FALLOCATE_FAIL\|DM_RELOAD\|DM_SUSPEND\|DM_RESUME\|KNOB_FAIL" \
		$T/umltest/fw-$PLAN-$a/log | grep -v FW_READ_BAD | sed "s/^/  [$a] /"
	echo "  [$a] FW_READ_BAD lines: $(grep -ac FW_READ_BAD $T/umltest/fw-$PLAN-$a/log)"
done
res() { cat $T/umltest/fw.fw-$PLAN-$1 2>/dev/null || echo "?"; }
read -r f_acked f_fl f_named f_torn f_queued f_rok f_ok f_eio f_rdok f_rdeio f_rdbad f_stale f_full f_drop \
	f_commit f_secs f_scrubbed f_fsync f_ro f_rold f_unn <<<"$(res fixed)"
read -r c_acked c_fl c_named c_torn c_queued c_rok c_ok c_eio c_rdok c_rdeio c_rdbad c_stale c_full c_drop \
	c_commit c_secs c_scrubbed c_fsync c_ro c_rold c_unn <<<"$(res control)"
case "$f_acked$c_acked" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/fw-$PLAN-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq WATCHDOG $T/umltest/fw-$PLAN-*/log && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
grep -lqs CONTROL_KNOB_FAIL $T/umltest/fw-$PLAN-control/log $T/umltest/fw-$PLAN-unnamedctl/log &&
	{ echo "RESULT: INCONCLUSIVE -- the control knob is not there (not a CONFIG_BTRFS_DEBUG kernel?)"; exit 2; }
if [ "$f_commit" != 0 ] || [ "$c_commit" != 0 ]; then
	echo "RESULT: INCONCLUSIVE -- a transaction commit ran while the device dropped writes"; exit 2
fi
if [ "${f_fl:-0}" = 0 ] || [ "${c_fl:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- no flush failed (flush_io_errs fixed=$f_fl control=$c_fl)"; exit 2
fi
if [ $PLAN = busy ]; then
	read -r fb_ok fb_eio fb_evh fb_ev fb_dr fb_tb fb_secs < $T/umltest/fw.busy.res.fw-busy-fixed 2>/dev/null ||
		fb_ok=?
	read -r cb_ok cb_eio cb_evh cb_ev cb_dr cb_tb cb_secs < $T/umltest/fw.busy.res.fw-busy-control 2>/dev/null ||
		cb_ok=?
	case "$fb_ok$cb_ok" in *'?'*) echo "RESULT: INCONCLUSIVE -- a busy phase did not report"; exit 2;; esac
	grep -lq "DM_SUSPEND_FAIL\|DM_RESUME_FAIL" $T/umltest/fw-busy-*/log &&
		{ echo "RESULT: INCONCLUSIVE -- device 1 could not be suspended or resumed"; exit 2; }
	echo "  possibly torn records before: fixed $fb_tb, control $cb_tb"
	echo "  writes at once ok/eio: fixed $fb_ok/$fb_eio (${fb_secs}s), control $cb_ok/$cb_eio (${cb_secs}s)"
	echo "  records spent while device 1 was held/in all, record_dropped:" \
	     "fixed $fb_evh/$fb_ev +$fb_dr, control $cb_evh/$cb_ev +$cb_dr"
	if [ "${f_torn:-0}" = 0 ] || [ "${fb_tb:-0}" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the readd marked nothing possibly torn"; exit 2
	fi
	if [ "${cb_ev:-0}" -lt 2 ]; then
		echo "RESULT: INCONCLUSIVE -- the control spent $cb_ev record(s): no two writes were"
		echo "        recorded at once, so a clean fixed arm proves nothing"
		exit 2
	fi
	if [ "$fb_ev" != 0 ] || [ "$fb_dr" != 0 ]; then
		echo "RESULT: FAIL -- $fb_ev possibly torn record(s) spent (record_dropped +$fb_dr) while a"
		echo "        write in flight could make room"
		exit 1
	fi
	if [ "$fb_eio" != 0 ] || [ "$fb_ok" = 0 ]; then
		echo "RESULT: FAIL -- $fb_eio of $((fb_ok + fb_eio)) writes failed waiting for the write in flight"
		exit 1
	fi
	echo "RESULT: PASS -- with a write in flight, $fb_ok writes into a log full of possibly torn"
	echo "        records waited for it and spent none; spent eagerly, $cb_ev were (control)"
	exit 0
fi
echo "  after the readd: stale_marks $f_named/$c_named, torn_blocks $f_torn/$c_torn," \
     "repairs asked for $f_queued/$c_queued, done $f_rok/$c_rok, device_write_failed +$f_stale/+$c_stale (fixed/control)"
echo "  new writes ok/eio: fixed $f_ok/$f_eio (${f_secs}s), control $c_ok/$c_eio (${c_secs}s);" \
     "log_full $f_full/$c_full, record_dropped $f_drop/$c_drop"
echo "  read back ok/eio/bad of acknowledged: fixed $f_rdok/$f_rdeio/$f_rdbad of $f_acked," \
     "control $c_rdok/$c_rdeio/$c_rdbad of $c_acked"
if [ $PLAN = unnamed ]; then
	echo "  the commit whose barrier failed: went on fixed $f_fsync control $c_fsync, read-only" \
	     "fixed $f_ro control $c_ro; log_flush_unnamed fixed $f_unn control $c_unn;" \
	     "blocks read back as before the write: fixed $f_rold control $c_rold"
	if [ "${c_torn:-0}" = 0 ] || [ "${c_fsync:-0}" != 1 ] || [ "${c_rold:-0}" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the control did not acknowledge an unnamed flush loss that reads"
		echo "        back old (torn_blocks $c_torn, commit went on $c_fsync, old $c_rold)"
		exit 2
	fi
	if [ "${f_fsync:-1}" != 0 ] || [ "${f_ro:-0}" != 1 ]; then
		echo "RESULT: FAIL -- the commit that could not name device 1 went on (sync $f_fsync, ro $f_ro)"
		exit 1
	fi
	if [ "${f_unn:-0}" = 0 ] || [ "${f_drop:-0}" != 0 ]; then
		echo "RESULT: FAIL -- log_flush_unnamed $f_unn, record_dropped $f_drop"; exit 1
	fi
	if [ $(( ${f_rdbad:-0} - ${f_rold:-0} )) != 0 ]; then
		echo "RESULT: FAIL -- $(( f_rdbad - f_rold )) block(s) read back as neither the write nor the old content"
		exit 1
	fi
	grep -aq "every transaction commit from here FAILS and the filesystem goes read-only" \
		$T/umltest/fw-$PLAN-fixed/log ||
		{ echo "RESULT: FAIL -- the log_flush_unnamed explanation does not say the commit failed"; exit 1; }
	# The names the refused readd could not write, kept for the reads.
	read -r u_acked _ _ _ _ _ _ _ u_rdok u_rdeio u_rdbad _ _ _ _ _ _ u_fsync u_ro u_rold u_unn \
		<<<"$(res unnamedctl)"
	echo "  unnamedctl: went on $u_fsync, read-only $u_ro, log_flush_unnamed $u_unn;" \
	     "read back ok/eio/bad $u_rdok/$u_rdeio/$u_rdbad of $u_acked (old $u_rold)"
	if [ "${u_fsync:-1}" != 0 ] || [ "${u_rold:-0}" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- with raid56_wf_refusal_leaves_unnamed=1 the commit went on or"
		echo "        nothing read back old, so the fixed arm's reads show nothing"
		exit 2
	fi
	if [ "${f_rold:-0}" != 0 ] || [ "${f_rdeio:-0}" != 0 ]; then
		echo "RESULT: FAIL -- after the refusal $f_rold block(s) read back as before the write and"
		echo "        $f_rdeio failed, although the refused readd could have kept their names"
		exit 1
	fi
	echo "RESULT: PASS -- the commit that could not name device 1 failed, read-only, with the alert,"
	echo "        and every overwrite read back as written until the unmount ($f_rdok);"
	echo "        keeping no names, $u_rold read back as before (unnamedctl); control: the commit went"
	echo "        on and $c_rold acknowledged block(s) read back old, no error"
	exit 0
fi
if [ $PLAN = named ]; then
	if [ "${f_named:-0}" = 0 ] || [ "${f_torn:-0}" != 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the readd did not name the device (stale_marks $f_named, torn_blocks $f_torn)"
		exit 2
	fi
else
	if [ "${f_torn:-0}" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the readd marked nothing possibly torn"; exit 2
	fi
fi
if [ $PLAN = torn ]; then
	if [ "${c_eio:-0}" != 0 ] || [ "${c_drop:-0}" = 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the control did not spend the records and keep writing"
		echo "        (eio $c_eio, record_dropped $c_drop), so it did not reproduce stage 0"
		exit 2
	fi
	if [ "${f_drop:-0}" != 0 ]; then
		echo "RESULT: FAIL -- possibly torn records were dropped (record_dropped $f_drop)"; exit 1
	fi
	if [ "${f_eio:-0}" = 0 ] || [ "${f_full:-0}" = 0 ]; then
		echo "RESULT: FAIL -- the log full of possibly torn records did not refuse (eio $f_eio, log_full $f_full)"
		exit 1
	fi
	grep -aq "retires the records that only say a write may have been torn" \
		$T/umltest/fw-$PLAN-fixed/log ||
		{ echo "RESULT: FAIL -- the log_full explanation does not say a scrub retires them"; exit 1; }
	if [ "$f_scrubbed" != ok ]; then
		echo "RESULT: FAIL -- after the scrub the alert asks for, a write into a new region still failed"
		exit 1
	fi
	echo "RESULT: PASS -- a log full of possibly torn records refused $f_eio of $((f_ok + f_eio)) new writes"
	echo "        (log_full $f_full) and dropped none; after a scrub a new write succeeded.  Control"
	echo "        (stage 0): spent with the alert (record_dropped $c_drop), every new write succeeded"
	exit 0
fi
if [ "${c_eio:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control did not wedge, so a clean fixed arm proves nothing"; exit 2
fi
grep -aq "btrfs scrub start <mountpoint>': it repairs every stripe it can" $T/umltest/fw-$PLAN-control/log ||
	{ echo "RESULT: FAIL -- the log_full explanation does not name scrub"; exit 1; }
if [ "$f_eio" != 0 ]; then
	echo "RESULT: FAIL -- the records the failed flush left wedged the log: $f_eio of $((f_ok + f_eio)) new writes failed"
	exit 1
fi
if [ $PLAN = named ]; then
	if [ "$f_rdbad" != 0 ] || [ "$c_rdbad" != 0 ]; then
		echo "RESULT: FAIL -- acknowledged blocks read back wrong: fixed $f_rdbad, control $c_rdbad"; exit 1
	fi
	if [ "${f_stale:-0}" = 0 ] || [ "${c_stale:-0}" != 0 ]; then
		echo "RESULT: FAIL -- device_write_failed fixed +$f_stale control +$c_stale: expected the alert only when repairs are asked for"
		exit 1
	fi
	if [ "${f_rok:-0}" = 0 ]; then
		echo "RESULT: FAIL -- no repair landed"; exit 1
	fi
	echo "RESULT: PASS -- the named stripes were repaired ($f_rok) and every new write succeeded ($f_ok),"
	echo "        every overwritten block reads back as written; without the repairs $c_eio of $((c_ok + c_eio)) failed"
	exit 0
fi
