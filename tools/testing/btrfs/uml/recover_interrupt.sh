#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does a read-only degraded mount made read-write go on refusing what it
# refused -- while its recovery runs, and after the recovery is interrupted?
#
#   recover_interrupt.sh <kernel>
#
# See degraded_crash_prep and degraded_crash (PRE=) and degraded_crash_kill in
# init-final3.sh.  RAID5 over three devices, raid1 metadata: a nodatacow file
# of 'P's, then degraded_crash.sh's file of 'A's, whose first whole full stripe
# is torn by a crash while the device of its column B is out.  Before the
# crash the 'P' file is rewritten while degraded, so the log records its
# stripes as well, in front of the torn one.  Then, B's device still out:
# mount -o ro,degraded, where column B's blocks of the torn stripe are refused
# (the parity may be torn, and nothing is left to check a rebuild of them),
# then mount -o remount,rw,degraded with the recovery lingering on each stripe,
# and column B read
#   window  while the recovery is on the first 'P' stripe
#   killed  after that remount is SIGKILLed (a Ctrl-C, a mount timeout): it
#           fails, and the filesystem stays mounted read-only
#   self    during a second remount,rw, while the recovery lingers on the
#           torn stripe itself: taken over from the log, not decided yet
#   retry   after that remount has run the recovery to the end
#   fixed    every read of column B is 'A' or EIO, and the interrupted
#            recovery says the refusals stay in force
#   control  raid56_wf_recover_drops_refusals=1: the recovery drops the
#            refusal before it reaches any stripe and never puts it back:
#            block 0 of column B reads back as the rebuild from the torn
#            parity, 'C's, with no error -- in the window and after the kill
#   selfctl  raid56_wf_recovering_stripe_readable=1: only the stripe the
#            recovery is deciding goes unrefused -- block 0 reads back so
#            in the self phase
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: recover_interrupt.sh <kernel>}
NDEV=3
PRE=512
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-rik.sh.$$ &&
	mv -f $T/umltest/init-rik.sh.$$ $T/umltest/init-rik.sh
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py.$$ &&
	mv -f $T/umltest/raid56_layout.py.$$ $T/umltest/raid56_layout.py
ulimit -c 0
boot() {	# tag mode omit opts [kernel args...]
	local tag=$1 mode=$2 omit=$3 opts=$4 ubds="" mnt="" d
	local D=$T/umltest/$tag
	shift 4
	for d in $(seq 0 $((NDEV-1))); do
		[ "$d" = "$omit" ] && continue
		ubds="$ubds ubd$d=$D/disk$d.img"
		[ -n "$mnt" ] || mnt=/dev/ubd$(echo abcdefgh | cut -c$((d + 1)))
	done
	timeout 1200 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-rik.sh $ubds quiet con=null con0=fd:0,fd:1 \
		BTRFS_TEST_DIR=$T MODE=$mode OPTS=$opts PROFILE=raid5:raid1 TAG=$tag \
		NDEV=$NDEV MNTDEV=$mnt PRE=$PRE "$@" < /dev/null > $D/log.$mode 2>&1
	echo "boot $mode omit=$omit rc=$?" >> $D/log.boots
}
arm() {	# name control
	local tag=rik-$1 idx_b
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/layout.$tag $T/umltest/rik.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done
	boot $tag degraded_crash_prep "" rw
	idx_b=$(sed -n 's/.*IDX_B=\([0-9]*\).*/\1/p' $T/umltest/layout.$tag 2>/dev/null)
	if [ -n "$idx_b" ]; then
		boot $tag degraded_crash "$idx_b" rw CRASH=1 NAMED=0
		boot $tag degraded_crash_kill "$idx_b" ro CONTROL=$2
	fi
	rm -f $D/disk*.img
}
arm fixed 0
arm control 1
arm selfctl 2
SHOW="layout:|crash armed|NO_CRASH|crash injection|pre: |RIK |WRONG|lingering|remount"
SHOW="$SHOW|recovery interrupted|recovery stopped|recovery done|control:"
SHOW="$SHOW|PRE_WRITE_FAIL|CHATTR_FAIL|LAYOUT_FAIL|KNOB_FAIL|KERNEL_SPLAT|WATCHDOG"
SHOW="$SHOW|MOUNT_FAIL|MKFS_FAIL"
for a in fixed control selfctl; do
	grep -ahE "$SHOW" $T/umltest/rik-$a/log.* 2>/dev/null | cut -c1-400 | sed "s/^/  [$a] /"
done
# lingering, remount rc, mounted, "recovery stopped" lines, remounted, and
# ok/eio/wrong of column B read-only, in the window, killed and after the
# retry, read_unverifiable, lingering on the torn stripe, ok/eio/wrong then
res() { cat $T/umltest/rik.rik-$1 2>/dev/null || echo "? ? ? ? ? ? ? ? ? ? ? ?"; }
read -r f_lin f_mrc f_mo f_stop f_mo2 f_ro f_win f_kill f_retry f_amb f_slin f_self \
	<<<"$(res fixed)"
read -r c_lin c_mrc c_mo c_stop c_mo2 c_ro c_win c_kill c_retry c_amb c_slin c_self \
	<<<"$(res control)"
read -r s_lin s_mrc s_mo s_stop s_mo2 s_ro s_win s_kill s_retry s_amb s_slin s_self \
	<<<"$(res selfctl)"
wrong() { echo "${1##*/}"; }
echo "  column B ok/eio/wrong:  read-only  window  killed  self  retry"
echo "    fixed                $f_ro  $f_win  $f_kill  $f_self  $f_retry" \
     "(remount rc $f_mrc, left $f_mo, then $f_mo2)"
echo "    control              $c_ro  $c_win  $c_kill  $c_self  $c_retry" \
     "(remount rc $c_mrc, left $c_mo, then $c_mo2)"
echo "    selfctl              $s_ro  $s_win  $s_kill  $s_self  $s_retry" \
     "(remount rc $s_mrc, left $s_mo, then $s_mo2)"
case "$f_lin$c_lin$s_lin" in
*'?'*) echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;;
esac
for a in fixed control selfctl; do
	grep -aq "crash injection 1 at full stripe" $T/umltest/rik-$a/log.degraded_crash \
		2>/dev/null && continue
	echo "RESULT: INCONCLUSIVE -- $a never crashed in the torn write"
	exit 2
done
if grep -alq KERNEL_SPLAT $T/umltest/rik-*/log.* 2>/dev/null; then
	echo "RESULT: FAIL -- kernel splat"
	exit 1
fi
if grep -alq "PRE_WRITE_FAIL\|KNOB_FAIL" $T/umltest/rik-*/log.* 2>/dev/null; then
	echo "RESULT: INCONCLUSIVE -- the 'P' file was not rewritten, or a knob is missing"
	exit 2
fi
for a in f c s; do
	lin=${a}_lin mrc=${a}_mrc mo=${a}_mo
	if [ "${!lin}" != 1 ] || [ "${!mrc}" = 0 ] || [ "${!mo}" != ro ]; then
		echo "RESULT: INCONCLUSIVE -- the remount was not caught inside the recovery" \
		     "and killed there (lingering ${!lin}, rc ${!mrc}, left ${!mo})"
		exit 2
	fi
done
if [ "$(wrong $c_win)" = 0 ] || [ "$(wrong $c_kill)" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- the control read nothing wrong (window $c_win," \
	     "killed $c_kill), so the refusal had nothing to hold back"
	exit 2
fi
if [ "$f_slin" != 1 ] || [ "$s_slin" != 1 ]; then
	echo "RESULT: INCONCLUSIVE -- the recovery was not caught lingering on the torn stripe" \
	     "(fixed $f_slin, selfctl $s_slin)"
	exit 2
fi
if [ "$(wrong $s_self)" = 0 ]; then
	echo "RESULT: INCONCLUSIVE -- selfctl read nothing wrong while the recovery decided" \
	     "the torn stripe ($s_self), so its refusal had nothing to hold back"
	exit 2
fi
for v in f_ro f_win f_kill f_self f_retry; do
	[ "$(wrong ${!v})" = 0 ] && continue
	echo "RESULT: FAIL -- a read of the torn stripe's missing column returned a" \
	     "rebuild nothing checked ($v ${!v})"
	exit 1
done
if [ "$f_mo2" != rw ]; then
	echo "RESULT: FAIL -- the remount after the interrupted one did not come up read-write"
	exit 1
fi
if [ "${f_stop:-0}" -lt 1 ]; then
	echo "RESULT: FAIL -- the interrupted recovery did not say the refusals stay in force"
	exit 1
fi
echo "RESULT: PASS -- the torn stripe's missing column stays refused while the recovery runs,"
echo "        decides it, and after it is interrupted (window $f_win, self $f_self, killed"
echo "        $f_kill, ok/eio/wrong); dropping the refusal up front read it wrong (window"
echo "        $c_win, killed $c_kill), and leaving the stripe being decided unrefused did"
echo "        (self $s_self)"
