#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# A tree-log replay that meets a write-intent log full of records naming a
# device: does the log_full alert give advice that works?
#
#   replay_full.sh <kernel>
#
# See replay_full in init-final3.sh.  RAID5 data and metadata, four devices.
# A file fsync'd (a tree log to replay), then device 1 fails writes while one
# block per region is overwritten in place where it holds the parity, until
# the log is full of the records naming it (82) and a write is refused
# (log_full); the machine stops.  Mounted again with device 1 working, the
# recovery keeps every record until after the replay (they are only checked
# while a tree log is to be replayed), and the replay's first write into a
# new region fails the mount with log_full.  Then what its explanation says:
#   fixed    to read the data, 'mount -o ro,rescue=nologreplay' (and the
#            refused write's explanation says the same): it mounts and the
#            overwritten blocks read back as written; to make the filesystem
#            writable, give the fsync'd changes up -- clear the tree log
#            ('btrfs rescue zero-log', from a btrfs-progs that knows the
#            raid56_write_intent feature: the rig's do not, and zero_log.py
#            does what one that does would) -- and mount again: its recovery
#            repairs the parity, the filesystem takes writes, every
#            overwritten block reads back as written
#   control  raid56_wf_advice_legacy=1: 'mount -o ro,nologreplay', which is
#            no mount option (the refused write's explanation, said in one
#            printk record, is cut off at 1024 bytes before its read-only
#            mount, '-o ro', which would replay the tree log and fail the
#            same way); and, the device working again, mount again: it fails
#            the same way
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: replay_full.sh <kernel>}
NDEV=4
FAIL=1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-rpf.sh.$$ && mv -f $T/umltest/init-rpf.sh.$$ $T/umltest/init-rpf.sh
cp $HERE/raid56_rows.py $T/umltest/raid56_rows.py.$$ &&
	mv -f $T/umltest/raid56_rows.py.$$ $T/umltest/raid56_rows.py
cp $HERE/zero_log.py $T/umltest/zero_log.py.$$ && mv -f $T/umltest/zero_log.py.$$ $T/umltest/zero_log.py
ulimit -c 0
arm() {	# name control
	local tag=rpf-$1 ubds="" i
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/rpf-prep.$tag $T/umltest/rpf-mount.$tag $T/umltest/rpf.*.$tag \
	      $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do
		truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"
	done
	# commit=600: no transaction commit, and so no barrier, drops the
	# overwrites' records before the crash (the guest checks and says so).
	timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw log_buf_len=4M \
		init=$T/umltest/init-rpf.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=replay_full OPTS=rw,commit=600 PROFILE=raid5:raid5 \
		TAG=$tag NDEV=$NDEV FAIL=$FAIL CONTROL=$2 PHASE=prep < /dev/null > $D/log.prep 2>&1
	timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw log_buf_len=4M \
		init=$T/umltest/init-rpf.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=replay_full OPTS=rw PROFILE=raid5:raid5 \
		TAG=$tag NDEV=$NDEV FAIL=$FAIL CONTROL=$2 PHASE=mount < /dev/null > $D/log.mount 2>&1
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
for a in fixed control; do
	grep -ahE "control:|RPF|KERNEL_SPLAT|WATCHDOG|_FAIL|dmesg: .*(tree-log replay|still full)" \
		$T/umltest/rpf-$a/log.* | cut -c1-300 | sed "s/^/  [$a] /"
done
res() { cat $T/umltest/rpf-$2.rpf-$1 2>/dev/null || echo "? ? ? ? ?"; }
read -r f_acked f_ref f_commit f_lroot f_rtopts <<<"$(res fixed prep)"
read -r c_acked c_ref c_commit c_lroot c_rtopts <<<"$(res control prep)"
read -r f_first f_said f_plan f_rw f_wr f_r f_again f_o1 f_ok1 f_r1 f_o2 f_ok2 f_r2 <<<"$(res fixed mount)"
read -r c_first c_said c_plan c_rw c_wr c_r c_again c_o1 c_ok1 c_r1 c_o2 c_ok2 c_r2 <<<"$(res control mount)"
echo "  prep: acknowledged fixed $f_acked control $c_acked, a write refused: fixed $f_ref control $c_ref;" \
     "tree log fixed $f_lroot control $c_lroot"
echo "  first mount: fixed $f_first (replay log_full said $f_said), control $c_first ($c_said)"
ro_said() { [ "$1" = none ] && echo "none given (cut off)" || echo "-o $1 mounted $2"; }
echo "  read-only as advised: fixed $(ro_said $f_o1 $f_ok1) ($f_r1), $(ro_said $f_o2 $f_ok2) ($f_r2);" \
     "control $(ro_said $c_o1 $c_ok1), $(ro_said $c_o2 $c_ok2)"
echo "  device working again, as advised: fixed $f_plan -> read-write $f_rw, takes writes $f_wr," \
     "overwrites B/eio/other $f_r; control $c_plan -> read-write $c_rw (replay failed again: $c_again)"
case "$f_acked$c_acked${f_first:-?}${c_first:-?}" in *'?'*)
	echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
grep -lq KERNEL_SPLAT $T/umltest/rpf-*/log.* && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq WATCHDOG $T/umltest/rpf-*/log.* && { echo "RESULT: FAIL -- a guest hung"; exit 1; }
grep -lq CONTROL_KNOB_FAIL $T/umltest/rpf-control/log.* &&
	{ echo "RESULT: INCONCLUSIVE -- the control knob is not there"; exit 2; }
if [ "$f_ref$c_ref" != 11 ] || [ "$f_commit$c_commit" != 00 ] ||
   [ "${f_lroot:-0}" = 0 ] || [ "${c_lroot:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the log never filled, a commit ran, or no tree log was left"
	exit 2
fi
# The reads below count the acknowledged overwrites: none, and they would
# pass having read nothing.
case "$f_acked:$c_acked" in ''|*[!0-9:]*|0:*|*:0)
	echo "RESULT: INCONCLUSIVE -- no overwrite was acknowledged (fixed $f_acked, control $c_acked)"
	exit 2;;
esac
if [ "$f_first$c_first" != failfail ] || [ "${f_said:-0}" = 0 ] || [ "${c_said:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the replay did not meet the full log (first mount fixed" \
	     "$f_first, control $c_first)"
	exit 2
fi
if [ "$c_plan" != remount ] || [ "$c_rw" != 0 ] || [ "${c_again:-0}" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control's advice did not fail the same way" \
	     "($c_plan -> read-write $c_rw)"
	exit 2
fi
if [ "$f_ok1" != 1 ] || [ "$f_ok2" != 1 ]; then
	echo "RESULT: FAIL -- a read-only mount the explanations give did not mount" \
	     "(-o $f_o1: $f_ok1, -o $f_o2: $f_ok2)"
	exit 1
fi
for r in $f_r1 $f_r2; do
	[ "$r" = "$f_acked/0/0" ] || { echo "RESULT: FAIL -- read-only, the overwrites read $r"; exit 1; }
done
if [ "$f_plan" != zerolog ] || [ "$f_rw" != 1 ] || [ "$f_wr" != 1 ]; then
	echo "RESULT: FAIL -- the advice for a device working again did not make the filesystem" \
	     "writable ($f_plan -> read-write $f_rw, takes writes $f_wr)"
	exit 1
fi
[ "$f_r" = "$f_acked/0/0" ] ||
	{ echo "RESULT: FAIL -- after the advised mount the overwrites read $f_r"; exit 1; }
echo "RESULT: PASS -- after a replay the full log failed, the read-only mount it advises mounts"
echo "        and clearing the tree log, as it advises, makes the filesystem writable with every"
echo "        block as written; control: $(ro_said $c_o1 $c_ok1), $(ro_said $c_o2 $c_ok2),"
echo "        and mounting again failed the same way"
exit 0
