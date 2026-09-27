#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Do the records a failed flush leaves wedge the write-intent log once it is
# full of them, with every device there?
#
#   flush_wedge.sh <kernel> [named|torn|busy|hot|unnamed|health]
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
#   torn   164 regions: too many to name, the readd marks them possibly torn,
#          which fills a narrow block
#     fixed    a full log keeps them: the new writes fail at once (log_full),
#              its explanation says a scrub retires them, and after one a
#              write into a new region succeeds; nothing is dropped
#     control  raid56_wf_evict_stage0=1: it spends them, last and with the
#              alert (record_dropped), and every new write succeeds
#   busy   163 regions, one slot left, and instead of FW_FRESH writes one at a
#          time, FW_BUSY (8) at once into new regions whose parity device 1
#          holds, started while device 1 is suspended: they queue behind it
#          and go together once it is back, the first to be recorded takes the
#          free slot, and the others find the log full with a write in flight
#          that frees a slot when it finishes
#     fixed    they wait for it and take its slot in turn: no possibly torn
#              record is spent, every write succeeds
#     control  raid56_wf_evict_stage0=1 and raid56_wf_torn_spent_eagerly=1:
#              each spends one at once (sticky_evicted, record_dropped)
#   hot    164 regions, as torn, and instead of FW_FRESH writes one at a
#          time, a write into one of the recorded regions held in flight once
#          its bios have completed (raid56_write_hold_ms), then one write into
#          a new region
#     fixed    it fails at once (log_full): the write in flight leaves a
#              record that may not be spent, and frees nothing
#     control  raid56_wf_room_any_write=1: it waits for the write in flight
#              to finish, then fails the same way
#   torn, busy and hot set raid56_wf_readd_acks_unnamed=1 in both arms: the
#   default refuses the commit that would leave such a log (unnamed).  busy
#   sets raid56_wf_admit_narrow=1 in both arms too: the default admits no
#   write into a log holding more of those records than a wide block
#   describes, and refuses all eight (commit_full.sh).
#   unnamed (CUR-4)  164 regions, as torn: too many to name device 1 in
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
#   health  unnamed's refused commit, then raid56_health on the read-only
#           filesystem
#     fixed    the log_flush_unnamed alert names device 1 (devid 2): the
#              devices line lists it, and the action is to unmount and mount
#              again first, then to replace it, scrub and acknowledge
#     control  raid56_wf_refusal_health_legacy=1: no device listed, and the
#              action a scrub, which a read-only filesystem refuses (EROFS)
#   Then the machine stops, and the filesystem is mounted again: the alert
#   comes back from the log (BTRFS_WIB_LATCH_OFFSET)
#     fixed    and so does devid 2 (BTRFS_WIB_LATCH_DEVS_OFFSET): the devices
#              line lists it, the action says to replace it
#     nodevs   raid56_wf_latch_no_devs=1: the alert without the device, which
#              nothing names any more -- the flush errors the device stats
#              counted never reached a disk
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: flush_wedge.sh <kernel> [named|torn|busy|hot|unnamed|health]}
PLAN=${2:-named}
case $PLAN in named|torn|busy|hot|unnamed|health) ;; *) echo "unknown plan $PLAN"; exit 2;; esac
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
	# health: the machine stopped after the refused commit; mount it again.
	[ $PLAN = health ] &&
		timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-fw.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=flush_wedge OPTS=rw PROFILE=raid5:raid1 \
			TAG=$tag NDEV=$NDEV FAIL=$FAIL PLAN=$PLAN CONTROL=$2 PHASE=reboot \
			< /dev/null > $D/log.reboot 2>&1
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
ARMS="fixed control"
[ $PLAN = unnamed ] && { arm unnamedctl 2; ARMS="$ARMS unnamedctl"; }
[ $PLAN = health ] && arm nodevs 2
if [ $PLAN = health ]; then
	grep -ah "control:\|FW \|alerts an earlier\|KERNEL_SPLAT\|WATCHDOG\|_FAIL" \
		$T/umltest/fw-$PLAN-nodevs/log* | sed "s/^/  [nodevs] /"
	for a in fixed control; do
		grep -ah "control:\|FW \|healed\|in-place overwrites\|alerts an earlier\|KERNEL_SPLAT\|WATCHDOG\|_FAIL" \
			$T/umltest/fw-$PLAN-$a/log* | sed "s/^/  [$a] /"
	done
	hres() { cat $T/umltest/fw.health.fw-$PLAN-$1 2>/dev/null || echo "?"; }
	read -r f_ok f_ro f_fl f_st f_un f_devs f_act <<<"$(hres fixed)"
	read -r c_ok c_ro c_fl c_st c_un c_devs c_act <<<"$(hres control)"
	read -r n_ok n_ro n_fl n_st n_un n_devs n_act <<<"$(hres nodevs)"
	rres() { cat $T/umltest/fw.reboot.fw-$PLAN-$1 2>/dev/null || echo "?"; }
	read -r f_rw f_run f_rdevs f_ract <<<"$(rres fixed)"
	read -r n_rw n_run n_rdevs n_ract <<<"$(rres nodevs)"
	echo "  after the refused commit: read-only fixed $f_ro control $c_ro; unacknowledged" \
	     "fixed $f_un control $c_un"
	echo "  devices: fixed $f_devs, control $c_devs"
	echo "  action: fixed $f_act, control $c_act"
	echo "  after the crash (mounted $f_rw/$n_rw): unacknowledged fixed $f_run, nodevs $n_run;" \
	     "devices fixed $f_rdevs, nodevs $n_rdevs; action fixed $f_ract, nodevs $n_ract"
	case "$f_ok$c_ok$n_ok$f_rw$n_rw" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
	grep -lq KERNEL_SPLAT $T/umltest/fw-$PLAN-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
	grep -lq WATCHDOG $T/umltest/fw-$PLAN-*/log && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
	grep -lq CONTROL_KNOB_FAIL $T/umltest/fw-$PLAN-{control,nodevs}/log* &&
		{ echo "RESULT: INCONCLUSIVE -- a control knob is not there"; exit 2; }
	for v in "$f_ok:$f_ro:$f_un" "$c_ok:$c_ro:$c_un" "$n_ok:$n_ro:$n_un"; do
		case "$v" in 0:1:*log_flush_unnamed*) ;; *)
			echo "RESULT: INCONCLUSIVE -- a commit was not refused read-only with" \
			     "log_flush_unnamed ($v)"; exit 2;;
		esac
	done
	case "$c_devs:$c_act" in none:scrub_then_ack) ;; *)
		echo "RESULT: INCONCLUSIVE -- the control did not reproduce the old advice" \
		     "(devices $c_devs, action $c_act)"; exit 2;;
	esac
	case ",$f_devs," in *,2:*) ;; *)
		echo "RESULT: FAIL -- the refusal's alert does not name devid 2 (devices $f_devs)"; exit 1;;
	esac
	case "$f_act" in unmount_then_mount-rw_then_replace-devid-2*_then_ack) ;; *)
		echo "RESULT: FAIL -- the action on the read-only filesystem is $f_act"; exit 1;;
	esac
	# After the crash: the alert is back from the log, and with it the device.
	for v in "$f_run" "$n_run"; do
		case ",$v," in *,log_flush_unnamed,*) ;; *)
			echo "RESULT: INCONCLUSIVE -- the alert was not back after the crash ($v)"; exit 2;;
		esac
	done
	case "$n_rdevs" in none) ;; *)
		echo "RESULT: INCONCLUSIVE -- raid56_wf_latch_no_devs: devices $n_rdevs after the crash"
		exit 2;;
	esac
	case ",$f_rdevs," in *,2:*) ;; *)
		echo "RESULT: FAIL -- after the crash raid56_health lists devices $f_rdevs, not devid 2"
		exit 1;;
	esac
	case "$f_ract" in *replace-devid-2*) ;; *)
		echo "RESULT: FAIL -- after the crash the action is $f_ract"; exit 1;;
	esac
	echo "RESULT: PASS -- after the refused commit raid56_health lists devid 2 and says to"
	echo "        unmount and mount again first, then replace it, and after the crash the"
	echo "        log still names devid 2 with the alert; controls: no device and a scrub"
	echo "        the read-only filesystem cannot run, no device after the crash"
	exit 0
fi
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
if [ $PLAN = hot ]; then
	read -r fh_held fh_rc fh_secs fh_hrc fh_full fh_drop fh_tb < $T/umltest/fw.hot.res.fw-hot-fixed \
		2>/dev/null || fh_held=?
	read -r ch_held ch_rc ch_secs ch_hrc ch_full ch_drop ch_tb < $T/umltest/fw.hot.res.fw-hot-control \
		2>/dev/null || ch_held=?
	case "$fh_held$ch_held" in *'?'*) echo "RESULT: INCONCLUSIVE -- a hot phase did not report"; exit 2;; esac
	grep -lq HOLD_KNOB_FAIL $T/umltest/fw-hot-*/log &&
		{ echo "RESULT: INCONCLUSIVE -- raid56_write_hold_ms is not there"; exit 2; }
	echo "  possibly torn records: fixed $fh_tb, control $ch_tb; writes held: fixed $fh_held, control $ch_held"
	echo "  the new write: fixed $fh_rc in ${fh_secs}s, control $ch_rc in ${ch_secs}s;" \
	     "the held write: fixed $fh_hrc, control $ch_hrc; log_full +$fh_full/+$ch_full," \
	     "record_dropped $fh_drop/$ch_drop"
	for a in fh ch; do
		eval "held=\$${a}_held tb=\$${a}_tb hrc=\$${a}_hrc rc=\$${a}_rc"
		if [ "${held:-0}" = 0 ] || [ "${tb:-0}" = 0 ] || [ "$hrc" != ok ] || [ "$rc" != eio ]; then
			echo "RESULT: INCONCLUSIVE -- $a: no write held ($held), no possibly torn record ($tb),"
			echo "        the held write failed ($hrc) or the new one went in ($rc)"
			exit 2
		fi
	done
	if [ "${ch_secs:-0}" -lt 10 ]; then
		echo "RESULT: INCONCLUSIVE -- the control failed the new write in ${ch_secs}s: it did not wait"
		echo "        for the write in flight, so a quick fixed arm proves nothing"
		exit 2
	fi
	if [ "${fh_secs:-99}" -gt 3 ] || [ "${fh_full:-0}" = 0 ] || [ "${fh_drop:-0}" != 0 ]; then
		echo "RESULT: FAIL -- the new write into a log full of records that must stay took ${fh_secs}s"
		echo "        to fail (log_full +$fh_full, record_dropped $fh_drop)"
		exit 1
	fi
	echo "RESULT: PASS -- with only a write into a recorded region in flight, a write into a new region"
	echo "        of a log full of possibly torn records failed in ${fh_secs}s (log_full); waiting for it"
	echo "        (control) took ${ch_secs}s to fail the same way"
	exit 0
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
