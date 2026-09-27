#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Does a refused repair clear the write-intent record's marks, so that a read
# of the stale column trusts a parity nothing rewrote?
#
#   refusal_marks.sh <kernel>
#
# See refusal_marks in init-final3.sh (the lens-fidelity badq scenario).
# RAID6 over four devices: a nodatacow block 'B' is overwritten while the
# devices of its column and of Q fail writes, so the record names the column
# stale and Q bad; Q is healed, the column's device still fails, and the
# queued repair's write-back of the column is refused.  Then the block is
# read cold.
#   norepair  raid56_repair_delay_ms=600000: no repair runs; must read 'B'
#             (the setup check)
#   default   the repair's write-back comes before it is recorded in flight,
#             and its refusal never reaches the record: 'B'
#   markfirst raid56_rmw_mark_before_repair=1: recorded first, the refusal
#             takes the path through the record's update, which leaves the
#             marks of a write that never wrote its data and parity alone:
#             'B'
#   control   markfirst and raid56_refusal_clears_marks=1: that update clears
#             Q's bad mark although Q was never rewritten; the read fails the
#             cross-check against the stale Q (EIO, read_parities_disagree)
#   trust     control and raid56_read_trusts_ambiguous=1: the read returns the
#             old 'A', with no error
# Since "record a write in flight only once its write-back is durable", the
# refusal_clears_marks knob alone reaches nothing here (the default arm): the
# path it restores only exists with the repair recorded first, so its control
# needs raid56_rmw_mark_before_repair=1 as well.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: refusal_marks.sh <kernel>}
NDEV=4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# Copied under a temporary name and renamed: a guest may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-rfm.sh.$$ && mv -f $T/umltest/init-rfm.sh.$$ $T/umltest/init-rfm.sh
cp $HERE/raid56_layout.py $T/umltest/raid56_layout.py.$$ &&
	mv -f $T/umltest/raid56_layout.py.$$ $T/umltest/raid56_layout.py
ulimit -c 0
arm() {	# name delay knobs...
	local tag=rfm-$1 delay=$2 ubds=""
	local D=$T/umltest/$tag
	shift 2
	rm -rf $D; mkdir -p $D; rm -f $T/umltest/rfm.$tag $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; ubds="$ubds ubd$i=$D/disk$i.img"; done
	timeout 900 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
		init=$T/umltest/init-rfm.sh $ubds quiet con=null con0=fd:0,fd:1 "$@" \
		BTRFS_TEST_DIR=$T MODE=refusal_marks OPTS=rw PROFILE=raid6:raid1c3 TAG=$tag \
		NDEV=$NDEV DELAY=$delay < /dev/null > $D/log 2>&1
	echo "boot rc=$?" >> $D/log
	rm -f $D/disk*.img
}
MF=btrfs.raid56_rmw_mark_before_repair=1
CL=btrfs.raid56_refusal_clears_marks=1
arm norepair 600000
arm default 1000
arm markfirst 1000 $MF
arm control 1000 $MF $CL
arm trust 1000 $MF $CL btrfs.raid56_read_trusts_ambiguous=1
ARMS="norepair default markfirst control trust"
for a in $ARMS; do
	grep -ahE "layout:|acknowledged|refused|RFM |could not write back|KERNEL_SPLAT|WATCHDOG|MOUNT_FAIL|MKFS_FAIL|LAYOUT_FAIL|DM_RELOAD" \
		$T/umltest/rfm-$a/log | cut -c1-220 | sed "s/^/  [$a] /"
done
res() { cat $T/umltest/rfm.rfm-$1 2>/dev/null || echo "? ? ? ?"; }
for a in $ARMS; do
	read -r b _ rc _ <<<"$(res $a)"
	case "$b" in '?') echo "RESULT: INCONCLUSIVE -- the $a arm did not report"; exit 2;; esac
done
grep -lq KERNEL_SPLAT $T/umltest/rfm-*/log && { echo "RESULT: FAIL -- kernel splat"; exit 1; }
grep -lq "B refused\|LAYOUT_FAIL" $T/umltest/rfm-*/log &&
	{ echo "RESULT: INCONCLUSIVE -- a write of B was refused or the layout not found"; exit 2; }
read -r n_b n_a n_rc n_f <<<"$(res norepair)"
read -r d_b d_a d_rc d_f <<<"$(res default)"
read -r m_b m_a m_rc m_f <<<"$(res markfirst)"
read -r c_b c_a c_rc c_f <<<"$(res control)"
read -r t_b t_a t_rc t_f <<<"$(res trust)"
echo "  B read back (bytes 'B' / bytes 'A' / dd rc / repairs failed): norepair $n_b/$n_a/$n_rc/$n_f," \
     "default $d_b/$d_a/$d_rc/$d_f, markfirst $m_b/$m_a/$m_rc/$m_f, control $c_b/$c_a/$c_rc/$c_f," \
     "trust $t_b/$t_a/$t_rc/$t_f"
[ "$n_b" = 4096 ] || { echo "RESULT: INCONCLUSIVE -- the no-repair arm did not read 'B' back, so the setup is wrong"; exit 2; }
for a in default markfirst control trust; do
	read -r _ _ _ f <<<"$(res $a)"
	[ "${f:-0}" -gt 0 ] ||
		{ echo "RESULT: INCONCLUSIVE -- no repair was refused in the $a arm"; exit 2; }
done
if [ "$c_b" = 4096 ] || [ "$t_b" = 4096 ]; then
	echo "RESULT: INCONCLUSIVE -- a control read 'B' back: the refused repair cleared nothing,"
	echo "        so the fixed arms' clean reads prove nothing"
	exit 2
fi
if [ "$d_b" != 4096 ] || [ "$d_rc" != 0 ]; then
	echo "RESULT: FAIL -- after the refused repair the block reads $d_b bytes of 'B' (rc $d_rc)"; exit 1
fi
if [ "$m_b" != 4096 ] || [ "$m_rc" != 0 ]; then
	echo "RESULT: FAIL -- recorded first, the refused repair left the block reading $m_b bytes of 'B'"
	echo "        (rc $m_rc)"
	exit 1
fi
echo "RESULT: PASS -- after the refused repair 'B' reads back, whether the repair was recorded"
echo "        before its write-back or after; clearing the marks, the read fails (control rc $c_rc)"
echo "        or returns $t_a bytes of the old 'A' (trust)"
