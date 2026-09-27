#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# K3: can a write-intent log end up holding more regions than a log block
# describes, and does a transaction commit then acknowledge names that exist
# only in memory?
#
#   commit_full.sh <kernel> [full|logio|replay]
#
# See commit_full in init-final3.sh.  RAID5 data, RAID1C3 metadata, four
# devices.  A crash leaves the log listing 100 regions in flight; mounted with
# a device left out (default 2), the recovery keeps every one of them, as
# verdicts and torn marks, which a narrow block describes.  Then a write into
# a new region whose column is on the missing device: its record names that
# column, which makes every block wide, and a wide block describes 82 regions.
#   fixed    the recovery keeps no more than a wide block holds
#            (wib_admit_max()): the read-write mount fails with
#            recovery_log_full, nothing is written, nothing reads wrong
#   r2       raid56_wf_admit_narrow=1: the mount goes read-write and the write
#            is made, but the commit that would acknowledge it cannot write the
#            log: it fails, read-only, with log_commit_failed, and the fsync
#            reports it; the log keeps the alert, and raid56_health still shows
#            it unacknowledged once every device is back
#   control  raid56_wf_admit_narrow=1 and raid56_wf_commit_keeps_previous=1:
#            the commit keeps the previous block and goes on; the write is
#            acknowledged with its name only in memory, and once every device
#            is back the recovery rebuilds the parity from the column the name
#            said was stale: the block reads back as it was before the write,
#            with no error
# INCONCLUSIVE unless the log listed more than 82 regions at the degraded
# mount and the control acknowledged the write and read it back old ('A',
# with no error: a refused read, or one of anything else, is not K3).
#
# Where the read-write mount is refused, the filesystem is mounted read-only,
# degraded, as recovery_log_full says, then remounted read-write, which is
# refused again; raid56_health is read after each:
#   fixed    recovery_log_full stands unacknowledged on the read-only mount
#            (the failed recovery wrote it to the log), and the action is
#            'bring-back-missing-devices then mount-rw then ack, else
#            copy-data-off-read-only then recreate', after the remount too
#   health   raid56_wf_recovery_full_legacy=1: nothing unacknowledged, and
#            the action 'mount-rw' -- the operation that was just refused
#            (with raid56_wf_latch_needs_log=1: the failed mount's latch work
#            writes the alert too, since a disabled log keeps acknowledgments)
#
# replay: as full, and a small file fsync'd before the overwrites, which leaves
# a tree log to replay.  The degraded read-write mount is refused
# (recovery_log_full); then the read-only mount its message gives:
#   fixed    'mount -o ro,degraded,rescue=nologreplay': it mounts and the data
#            reads; the message says that if the device cannot come back no
#            mount can make the filesystem writable again, and to copy the
#            data off and recreate it
#   advice   raid56_wf_advice_legacy=1: '-o ro, with -o degraded': that mount
#            replays the tree log, runs the recovery again and fails the same
#            way, and nothing says the filesystem cannot be made writable
#
# CF_KNOBS in the environment go on every boot's kernel command line.
#
# logio: the log block does not reach enough devices -- a write goes
# through, then the log slots of three of the four fail every IO, nothing
# else does (the barriers succeed); the transaction commit that drops the
# finished write's record cannot write the log
#   fixed    it fails, read-only, with log_commit_failed
#   control  raid56_wf_commit_keeps_previous=1: taken for a failed lazy
#            commit, it goes on
#
# disable: logio, with the log being disabled (echo 0 > features/
# raid56_write_intent) before the write: its first commit writes the superblock
# without the flag, and the commit that meets the failing log slots is the one
# that writes the log's last block (wib_write_final_locked())
#   fixed    it fails, read-only, with log_commit_failed, as any commit that
#            cannot write the log: the next mount reads the previous block,
#            flag or not, and the names the set gained since are only in memory
#   control  raid56_wf_commit_keeps_previous=1: "could not write the final log
#            block", and it goes on
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: commit_full.sh <kernel> [full|logio|disable|replay]}
PLAN=${2:-full}
case $PLAN in full|logio|disable|replay) ;; *) echo "unknown plan $PLAN"; exit 2;; esac
OMIT=2
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-cf.sh.$$ && mv -f $T/umltest/init-cf.sh.$$ $T/umltest/init-cf.sh
cp $HERE/raid56_rows.py $T/umltest/raid56_rows.py.$$ &&
	mv -f $T/umltest/raid56_rows.py.$$ $T/umltest/raid56_rows.py
ulimit -c 0
arm() {	# name control [final]
	local tag=cf-$1 control=$2 final=${3:-final}
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/cf-prep.$tag $T/umltest/cf-deg.$tag $T/umltest/cf-final.$tag \
	      $T/umltest/cf.rows.$tag $T/umltest/cf.target.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot() {	# phase omit opts
		local ubds="" mnt="" d
		for d in $(seq 0 $((NDEV-1))); do
			[ "$d" = "$2" ] && continue
			ubds="$ubds ubd$d=$D/disk$d.img"
			[ -n "$mnt" ] || mnt=/dev/ubd$(echo abcdefgh | cut -c$((d + 1)))
		done
		# log_buf_len: the recovery's messages about 100 stripes would push
		# out the count of regions the log listed.
		timeout 1800 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw log_buf_len=4M \
			init=$T/umltest/init-cf.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=commit_full OPTS=$3 PROFILE=raid5:raid1c3 TAG=$tag \
			NDEV=$NDEV CONTROL=$control MNTDEV=$mnt OMITTED=$OMIT PHASE=$1 PLAN=$PLAN \
			${CF_KNOBS:-} < /dev/null > $D/log.$1 2>&1
		echo "boot $1 rc=$?" >> $D/log.boots
	}
	# commit=600: no transaction commit drops the overwrites' records
	# before the crash (the guest checks and says so).
	boot prep none rw,commit=600
	boot degraded $OMIT rw
	[ $final = final ] && boot final none rw
	rm -f $D/disk*.img
}
if [ $PLAN = logio ] || [ $PLAN = disable ]; then
	DIS=0; [ $PLAN = disable ] && DIS=1
	for a in fixed:0 control:2; do
		tag=cf-$PLAN-${a%%:*}; D=$T/umltest/$tag
		rm -rf $D; mkdir -p $D; rm -f $T/umltest/cf-logio.$tag $T/umltest/results.$tag
		ubds=""
		for i in $(seq 0 $((NDEV-1))); do
			truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"
		done
		timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-cf.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=commit_full OPTS=rw,commit=600 PROFILE=raid5:raid1c3 \
			TAG=$tag NDEV=$NDEV CONTROL=${a##*:} PHASE=logio DISABLE=$DIS \
			< /dev/null > $D/log 2>&1
		rm -f $D/disk*.img
		grep -ahE "control:|CF |disable|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|KNOB_FAIL|DM_RELOAD" \
			$D/log | cut -c1-300 | sed "s/^/  [${a%%:*}] /"
	done
	res() { cat $T/umltest/cf-logio.cf-$PLAN-$1 2>/dev/null || echo '? ? ? ? ?'; }
	read -r fw fc fro flcf fdis <<<"$(res fixed)"
	read -r cw cc cro clcf cdis <<<"$(res control)"
	echo "  write: fixed $fw, control $cw; commit: fixed $fc (read-only $fro, log_commit_failed $flcf)," \
	     "control $cc (read-only $cro, log_commit_failed $clcf)"
	case "$fw$cw$fc$cc" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
	grep -lq KERNEL_SPLAT $T/umltest/cf-$PLAN-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
	grep -lq CONTROL_KNOB_FAIL $T/umltest/cf-$PLAN-*/log &&
		{ echo "RESULT: INCONCLUSIVE -- the control knob is not there"; exit 2; }
	if [ "$fw" != ok ] || [ "$cw" != ok ]; then
		echo "RESULT: INCONCLUSIVE -- the write before the fault failed (fixed $fw, control $cw)"
		exit 2
	fi
	# The disable: the log was still enabled at the write, and the commit
	# that met the failing slots was the one writing its last block.
	lazy="lazy commit failed"
	if [ $PLAN = disable ]; then
		lazy="could not write the final log block"
		if grep -lq "CF_DISABLED_EARLY\|DISABLE_FAIL" $T/umltest/cf-$PLAN-*/log ||
		   [ "$fdis" != 1 ] || [ "$cdis" != 1 ]; then
			echo "RESULT: INCONCLUSIVE -- the commit that met the failing log slots did not write"
			echo "        the last block of the log being disabled (fixed $fdis, control $cdis)"
			exit 2
		fi
	fi
	if [ "$cc" != ok ] || [ "$cro" != 0 ] ||
	   ! grep -aq "$lazy" $T/umltest/cf-$PLAN-control/log; then
		echo "RESULT: INCONCLUSIVE -- the control's commit did not meet the failing log slots"
		echo "        and go on (commit $cc, read-only $cro)"
		exit 2
	fi
	if [ "$fc" != fail ] || [ "$fro" != 1 ] || [ "${flcf:-0}" -lt 1 ]; then
		echo "RESULT: FAIL -- the commit whose log block reached too few devices went on"
		echo "        (commit $fc, read-only $fro, log_commit_failed $flcf)"
		exit 1
	fi
	grep -aq "a transaction commit FAILED and the filesystem is now read-only" \
		$T/umltest/cf-$PLAN-fixed/log ||
		{ echo "RESULT: FAIL -- no log_commit_failed explanation"; exit 1; }
	if [ $PLAN = disable ]; then
		echo "RESULT: PASS -- the commit whose last log block of a disable reached too few devices"
		echo "        failed, read-only, with log_commit_failed; control: it went on"
		exit 0
	fi
	echo "RESULT: PASS -- the commit whose log block reached too few devices failed, read-only,"
	echo "        with log_commit_failed; control: it went on"
	exit 0
fi
if [ $PLAN = replay ]; then
	# Tags of their own: the full plan's arms are cf-fixed and the like.
	arm replay-fixed 0 no
	arm replay-advice 4 no
	for a in fixed advice; do
		grep -ahE "control:|CF |KERNEL_SPLAT|WATCHDOG|_FAIL|could not keep the record" \
			$T/umltest/cf-replay-$a/log.* | cut -c1-300 | sed "s/^/  [$a] /"
	done
	res() { cat $T/umltest/cf-$2.cf-replay-$1 2>/dev/null || echo "? ? ? ? ?"; }
	read -r f_ov f_commit f_lroot <<<"$(res fixed prep)"
	read -r a_ov a_commit a_lroot <<<"$(res advice prep)"
	read -r f_ref f_opts f_ok f_got f_dead <<<"$(res fixed deg)"
	read -r a_ref a_opts a_ok a_got a_dead <<<"$(res advice deg)"
	echo "  tree log to replay: fixed $f_lroot, advice $a_lroot; read-write mount refused:" \
	     "fixed $f_ref, advice $a_ref"
	echo "  the read-only mount recovery_log_full gives: fixed '-o $f_opts' mounted $f_ok" \
	     "(target $f_got), advice '-o $a_opts' mounted $a_ok"
	echo "  says no mount can make it writable without the device: fixed $f_dead, advice $a_dead"
	case "$f_ov$a_ov$f_ref$a_ref" in *'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
	grep -lq KERNEL_SPLAT $T/umltest/cf-replay-{fixed,advice}/log.* &&
		{ echo "RESULT: FAIL -- kernel splat"; exit 1; }
	grep -lq WATCHDOG $T/umltest/cf-replay-{fixed,advice}/log.* &&
		{ echo "RESULT: FAIL -- a guest hung"; exit 1; }
	grep -lq CONTROL_KNOB_FAIL $T/umltest/cf-replay-advice/log.* &&
		{ echo "RESULT: INCONCLUSIVE -- the control knob is not there"; exit 2; }
	if [ "${f_lroot:-0}" = 0 ] || [ "${a_lroot:-0}" = 0 ] || [ "$f_commit$a_commit" != 00 ]; then
		echo "RESULT: INCONCLUSIVE -- no tree log was left to replay, or a commit ran"
		exit 2
	fi
	if [ "$f_ref$a_ref" != 11 ]; then
		echo "RESULT: INCONCLUSIVE -- the degraded read-write mount was not refused"; exit 2
	fi
	if [ "$a_opts" != ro,degraded ] || [ "$a_ok" != 0 ] || [ "${a_dead:-0}" != 0 ]; then
		echo "RESULT: INCONCLUSIVE -- the control's advice did not fail the same way"
		echo "        ('-o $a_opts' mounted $a_ok, dead end said $a_dead)"
		exit 2
	fi
	if [ "$f_opts" != ro,degraded,rescue=nologreplay ] || [ "$f_ok" != 1 ]; then
		echo "RESULT: FAIL -- the read-only mount recovery_log_full gives ('-o $f_opts')" \
		     "mounted $f_ok"
		exit 1
	fi
	case $f_got in A|eio) ;; *) echo "RESULT: FAIL -- the target block reads $f_got"; exit 1;; esac
	if [ "${f_dead:-0}" = 0 ]; then
		echo "RESULT: FAIL -- recovery_log_full does not say that without the device no mount"
		echo "        makes the filesystem writable again"
		exit 1
	fi
	echo "RESULT: PASS -- with a tree log to replay, the read-only mount recovery_log_full"
	echo "        gives (-o $f_opts) mounted, and it says the filesystem cannot be made"
	echo "        writable without the device; control: '-o ro,degraded' failed the same way"
	exit 0
fi
arm fixed 0
arm health 3 no
arm r2 1
arm control 2
SHOW="control:|CF_|CF |KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|LAYOUT_FAIL|KNOB_FAIL"
SHOW="$SHOW|block full|failing the transaction commit|could not keep the record"
for a in fixed health r2 control; do
	grep -ahE "$SHOW" $T/umltest/cf-$a/log.* | cut -c1-300 | sed "s/^/  [$a] /"
done
res() { cat $T/umltest/cf-$2.cf-$1 2>/dev/null || echo "? ? ? ? ? ?"; }
read -r f_ov f_commit _ <<<"$(res fixed prep)"
read -r r_ov r_commit _ <<<"$(res r2 prep)"
read -r c_ov c_commit _ <<<"$(res control prep)"
read -r f_regs f_ref f_ack f_ro f_lcf f_rlf f_roun f_roact f_rmact <<<"$(res fixed deg)"
read -r h_regs h_ref h_ack h_ro h_lcf h_rlf h_roun h_roact h_rmact <<<"$(res health deg)"
read -r r_regs r_ref r_ack r_ro r_lcf r_rlf <<<"$(res r2 deg)"
read -r c_regs c_ref c_ack c_ro c_lcf c_rlf <<<"$(res control deg)"
read -r f_got f_bad _ <<<"$(res fixed final)"
read -r r_got r_bad r_unack <<<"$(res r2 final)"
read -r c_got c_bad _ <<<"$(res control final)"
echo "  regions listed at the degraded mount: fixed $f_regs, r2 $r_regs, control $c_regs"
echo "  degraded read-write mount refused: fixed $f_ref, r2 $r_ref, control $c_ref"
echo "  target write acknowledged: r2 $r_ack, control $c_ack; read-only after it: r2 $r_ro," \
     "control $c_ro; log_commit_failed r2 $r_lcf, control $c_lcf"
echo "  target block once every device is back: fixed $f_got, r2 $r_got, control $c_got;" \
     "overwritten blocks read neither A nor B: $f_bad/$r_bad/$c_bad"
echo "  health on the read-only mount after the refused one: fixed $f_roun, $f_roact;" \
     "after a refused remount $f_rmact"
echo "  control (raid56_wf_recovery_full_legacy): $h_roun, $h_roact; after the remount $h_rmact"
case "$f_ov$r_ov$c_ov$f_regs$r_regs$c_regs$f_got$r_got$c_got$h_regs" in
*'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;;
esac
grep -lq KERNEL_SPLAT $T/umltest/cf-{fixed,health,r2,control}/log.* &&
	{ echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq WATCHDOG $T/umltest/cf-{fixed,health,r2,control}/log.* &&
	{ echo "RESULT: FAIL -- a guest hung"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/cf-{fixed,health,r2,control}/log.* && {
	echo "RESULT: INCONCLUSIVE -- a raid56_wf_* knob is not there"
	echo "        (not a CONFIG_BTRFS_DEBUG kernel?)"; exit 2
}
if [ "$f_commit$r_commit$c_commit" != 000 ]; then
	echo "RESULT: INCONCLUSIVE -- a transaction commit ran during the overwrites"; exit 2
fi
for n in $f_regs $r_regs $c_regs; do
	[ "$n" -gt 82 ] || {
		echo "RESULT: INCONCLUSIVE -- the log listed $n regions at a degraded mount, no more"
		echo "        than a wide block describes (82)"; exit 2
	}
done
if [ "$c_ref" != 0 ] || [ "$c_ack" != 1 ] || [ "$c_got" != A ]; then
	echo "RESULT: INCONCLUSIVE -- the control did not mount read-write, acknowledge the write"
	echo "        and read it back old (refused $c_ref, acknowledged $c_ack, reads $c_got):"
	echo "        it did not reproduce K3"
	exit 2
fi
bad=0
for v in $f_bad $r_bad; do [ "$v" = 0 ] || bad=1; done
[ $bad = 0 ] || { echo "RESULT: FAIL -- overwritten blocks read neither A nor B"; exit 1; }
# The read-only mount after the refused one counts from zero: the refused
# mount's alert is in its kernel log.
if [ "$f_ref" != 1 ] ||
   ! grep -aq "could not keep the record of full stripe" $T/umltest/cf-fixed/log.degraded; then
	echo "RESULT: FAIL -- with more regions to keep than a wide block holds, the degraded"
	echo "        read-write mount was not refused with recovery_log_full (refused $f_ref)"
	exit 1
fi
case $f_got in A) ;; *) echo "RESULT: FAIL -- fixed: the target block reads $f_got, not A"; exit 1;; esac
if [ "$h_ref" != 1 ] || [ "$h_roun" != none ] || [ "$h_roact" != mount-rw ] ||
   [ "$h_rmact" != mount-rw ]; then
	echo "RESULT: INCONCLUSIVE -- the health control did not show the old raid56_health"
	echo "        (refused $h_ref, unacknowledged $h_roun, action $h_roact / $h_rmact)"
	exit 2
fi
case ",$f_roun," in *,recovery_log_full,*) ;; *)
	echo "RESULT: FAIL -- the read-only mount after the refused one shows $f_roun unacknowledged"
	exit 1;;
esac
for a in "$f_roact" "$f_rmact"; do
	case "$a" in bring-back-missing-devices_then_mount-rw_then_ack,_else_copy-data-off*) ;; *)
		echo "RESULT: FAIL -- the action after recovery_log_full is $a"; exit 1;;
	esac
done
if [ "$r_ref" != 0 ]; then
	echo "RESULT: INCONCLUSIVE -- r2: the degraded read-write mount was refused under"
	echo "        raid56_wf_admit_narrow=1, so no commit met the full log"
	exit 2
fi
if [ "$r_ack" != 0 ] || [ "$r_ro" != 1 ] || [ "${r_lcf:-0}" -lt 1 ]; then
	echo "RESULT: FAIL -- r2: the commit that could not write the log did not fail it"
	echo "        (acknowledged $r_ack, read-only $r_ro, log_commit_failed $r_lcf)"
	exit 1
fi
grep -aq "a transaction commit FAILED and the filesystem is now read-only" \
	$T/umltest/cf-r2/log.degraded ||
	{ echo "RESULT: FAIL -- r2: no log_commit_failed explanation"; exit 1; }
case $r_got in A|C) ;; *) echo "RESULT: FAIL -- r2: the target block reads $r_got"; exit 1;; esac
case ",$r_unack," in *,log_commit_failed,*) ;; *)
	echo "RESULT: FAIL -- r2: after the reboot raid56_health shows $r_unack unacknowledged,"
	echo "        not log_commit_failed"
	exit 1;;
esac
echo "RESULT: PASS -- with $f_regs regions listed, the degraded read-write mount was refused"
echo "        (recovery_log_full), the read-only mount showed it and said to bring the device"
echo "        back or copy the data off, and nothing read wrong; admitted as before, the commit that"
echo "        could not write the name failed, read-only, log_commit_failed, fsync EIO, and"
echo "        the alert stood after the reboot;"
echo "        control: acknowledged with the name only in memory, read back $c_got (K3)"
