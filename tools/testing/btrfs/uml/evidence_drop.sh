#!/bin/bash
# What does the evidence channel do when it CANNOT keep a stripe?
#
#   evidence_drop.sh <kernel> [ndev]
#
# The channel copies the data columns of every full stripe a scrub declines to
# repair, into a ring of four slots that a helper drains while the scrub runs.
# Three things can stop a stripe being kept, and until now none of them had
# ever executed:
#
#   ring full   the helper is not keeping up.  The ring deliberately does NOT
#               overwrite the oldest entry -- that is the one the helper is
#               about to read -- so the newest is refused and dropped_full
#               counts it.  Silently discarding here would be the one failure
#               this whole channel exists to prevent, so the count has to be
#               real and reportable.
#   too wide    a stripe with more columns than the header can name (34) or
#               more data than a slot can hold (sixteen columns' worth, which
#               an 18-device RAID5 exceeds).  UML tops out at 16 block
#               devices, so neither is reachable here; this arm lowers the
#               column limit with btrfs.raid56_evidence_max_cols and drives
#               the first of the two, rather than pretending the path does
#               not exist.
#   disarmed    a helper arming and disarming while captures are in flight.
#               The channel's lock used to live inside the object the disarm
#               frees, so a capture could take a lock in freed memory; and a
#               capture that found the channel gone returned WITHOUT releasing
#               that lock, leaving the disarm blocked on it forever.  Neither
#               is visible from userspace as anything but a hang or a splat,
#               so this arm asserts that the scrub finishes and the kernel is
#               quiet.
#
# Every arm builds the same state as nocow_persist.sh's ambiguous case -- a
# device failing writes, every parity recorded unusable -- because that is what
# makes a scrub decline, which is what makes a capture happen.
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: evidence_drop.sh <kernel> [ndev]}
NDEV=${2:-4}
FAIL=1
PROFILE=raid5:raid1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
cp $HERE/init-final3.sh $T/umltest/init-final3.sh
cc -O2 -o $T/umltest/evidence $HERE/../evidence.c 2>/dev/null || true
cc -O2 -o $T/umltest/wibdump $HERE/../wibdump.c 2>/dev/null || true
[ -x $T/umltest/evidence ] || { echo "RESULT: INCONCLUSIVE -- no compiler for the consumer"; exit 2; }
ulimit -c 0

# More ambiguous full stripes than the ring has slots, so "the ring filled up"
# is something the workload can actually produce.
BLOCKS=${BLOCKS:-96}
SIZEMB=${SIZEMB:-8}

arm() {	# tag evidence-mode [extra-cmdline]
	local tag=evdrop-$1 mode=$2 cmdline="${3:-}" d ubds="" D
	D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/results.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done

	boot() {	# mode extra
		ubds=""
		for d in $(seq 0 $((NDEV-1))); do ubds="$ubds ubd$d=$D/disk$d.img"; done
		timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-final3.sh $ubds quiet con=null con0=fd:0,fd:1 \
			$cmdline BTRFS_TEST_DIR=$T MODE=$1 OPTS=rw PROFILE=$PROFILE TAG=$tag \
			MNTDEV=/dev/mapper/d0 NDEV=$NDEV FAIL=$FAIL \
			NOCOW_BLOCKS=$BLOCKS NOCOW_SIZE_MB=$SIZEMB ${2:-} \
			> $D/log.$1 2>&1
		echo "boot $1 rc=$?" >> $T/umltest/results.$tag
	}

	boot nocow_persist_prep  "FAKEBADPAR=1"
	boot nocow_persist_scrub "EVIDENCE=$mode"
}

# echo "<captured> <dropped_full> <dropped_wide> <waited>" for an arm, or "? ? ? ?"
# init-final3.sh's log() appends to results.<tag> AND echoes to the console,
# which the host captures into the per-boot log.  Those are not always both
# populated -- a boot whose console output never flushed leaves an empty
# per-boot log behind a perfectly complete results file -- so read both rather
# than report "did not run" for a run that did.
stats_of() {
	local l
	l=$(grep -ha 'EVIDENCE_STATS' $T/umltest/results.evdrop-$1 \
		$T/umltest/evdrop-$1/log.* 2>/dev/null | tail -1)
	[ -n "$l" ] || { echo "? ? ? ?"; return; }
	echo "$l" | sed -n 's/.*captured=\([0-9]*\) dropped_full=\([0-9]*\) dropped_wide=\([0-9]*\) waited=\([0-9]*\).*/\1 \2 \3 \4/p'
}
rc_of() { grep -ha "boot nocow_persist_scrub rc=" $T/umltest/results.evdrop-$1 2>/dev/null | sed 's/.*rc=//'; }
# mkfs.btrfs greets every RAID5/6 filesystem with "WARNING: RAID5/6 support has
# known problems", so a bare WARNING: cannot be the test.  Match what the
# kernel actually prints for a splat, which is what regress.sh matches too.
splat_of() {
	grep -hac -E "BUG:|KASAN|Oops|possible circular|hung task|INFO: task|WARNING: CPU|WARNING: at|WARNING: possible" \
		$T/umltest/results.evdrop-$1 $T/umltest/evdrop-$1/log.* 2>/dev/null |
		awk '{s+=$1} END {print s+0}'
}

echo "== drained continuously, capture waits for a slot (the workflow) =="
arm drain 1
read -r d_cap d_full d_wide d_wait <<<"$(stats_of drain)"
echo "  captured=$d_cap dropped_full=$d_full dropped_wide=$d_wide waited=$d_wait"

echo "== control: drained the same, but the capture will not wait =="
arm nowait 1 "btrfs.raid56_evidence_wait_ms=0"
read -r n_cap n_full n_wide n_wait <<<"$(stats_of nowait)"
echo "  captured=$n_cap dropped_full=$n_full dropped_wide=$n_wide waited=$n_wait"

echo "== never drained (ring must fill and count the rest) =="
arm full 2
read -r f_cap f_full f_wide f_wait <<<"$(stats_of full)"
echo "  captured=$f_cap dropped_full=$f_full dropped_wide=$f_wide waited=$f_wait"

echo "== every stripe too wide =="
arm wide 3 "btrfs.raid56_evidence_max_cols=1"
read -r w_cap w_full w_wide w_wait <<<"$(stats_of wide)"
echo "  captured=$w_cap dropped_full=$w_full dropped_wide=$w_wide waited=$w_wait"

# The capture delay makes this deterministic rather than hopeful: every claimed
# slot is held for two seconds before it is published, and the helper is arming
# and disarming every few hundred microseconds, so a disarm lands inside that
# window on every stripe.  That is the exact interleaving the channel's lock
# was moved out of the object for.
echo "== armed and disarmed INSIDE every capture =="
arm race 4 "btrfs.raid56_evidence_capture_delay_ms=2000"
echo "  scrub boot rc=$(rc_of race)  rounds=$(sed -n 's/.*EVIDENCE_RACE_ROUNDS \([0-9]*\).*/\1/p' \
	$T/umltest/results.evdrop-race 2>/dev/null | tail -1)"
echo

fails=0
say() { echo "  $1"; }
bad() { echo "  FAIL: $1"; fails=$((fails+1)); }

case "$d_cap$n_cap$f_cap$w_cap" in *'?'*)
	echo "RESULT: INCONCLUSIVE -- an arm did not report its counters"; exit 2;;
esac

# How many stripes the scrub declined at all.  Every arm builds the same state,
# and the too-wide arm refuses all of them without ever touching the ring, so
# its dropped_wide is the cleanest count of how many there were.
total=${w_wide:-0}
[ "$total" -gt 4 ] 2>/dev/null || {
	echo "RESULT: INCONCLUSIVE -- only $total stripe(s) were declined, which a"
	echo "        four-slot ring holds without overflowing.  Raise BLOCKS/SIZEMB."
	exit 2
}

# The control is what makes the arm above mean anything: same workload, same
# draining, only the willingness to wait removed.
if [ "${n_full:-0}" -eq 0 ] 2>/dev/null; then
	echo "RESULT: INCONCLUSIVE -- the no-wait control lost nothing either, so a"
	echo "        clean result with waiting proves nothing"
	exit 2
fi
[ "${d_full:-0}" -eq 0 ] 2>/dev/null || bad "$d_full stripe(s) were still dropped while a helper was draining"
[ "${d_cap:-0}" -eq "$total" ] 2>/dev/null || bad "draining kept $d_cap of $total declined stripe(s)"
[ "${d_wait:-0}" -gt 0 ] 2>/dev/null || bad "no capture ever waited for a slot, so the wait path did not execute"

if [ "${f_full:-0}" -eq 0 ] 2>/dev/null; then
	echo "RESULT: INCONCLUSIVE -- not draining still overflowed nothing, so the"
	echo "        ring-full path did not execute.  Raise BLOCKS/SIZEMB."
	exit 2
fi
[ "${f_cap:-0}" -le 4 ] 2>/dev/null || bad "the ring kept $f_cap stripe(s) with nothing draining it, but it only has 4 slots"
[ "${f_cap:-0}" -gt 0 ] 2>/dev/null || bad "the ring filled without keeping anything"
# A dead helper must cost ONE grace period, not one per declined stripe.  Not
# zero either: if no capture ever slept, ->stalled was not what bounded it and
# this proves nothing.
[ "${f_wait:-0}" -eq 1 ] 2>/dev/null || bad "with nothing draining, $f_wait capture(s) waited out the grace period; exactly one should have"


[ "${w_wide:-0}" -gt 0 ] 2>/dev/null || bad "no stripe was refused for width even with the limit at one column"
[ "${w_cap:-0}" -eq 0 ] 2>/dev/null || bad "a stripe was copied even though every stripe was too wide"

[ "$(rc_of race)" = "0" ] || bad "the scrub boot did not finish while the channel was being armed and disarmed under it"
for a in drain nowait full wide race; do
	[ "$(splat_of $a)" = "0" ] || bad "$a arm produced a kernel splat"
done

echo
if [ $fails -ne 0 ]; then
	echo "RESULT: FAIL -- $fails check(s) failed"; exit 1
fi
echo "RESULT: PASS -- a draining helper keeps all $d_cap declined stripe(s), $d_wait of"
echo "        them only because the capture waited; the same run without waiting"
echo "        loses $n_full.  With nothing draining, the ring keeps $f_cap, counts"
echo "        $f_full dropped rather than overwriting, and waits out the grace"
echo "        period $f_wait time(s) rather than once per stripe.  A stripe too wide"
echo "        to copy is refused and counted ($w_wide), not half-copied.  Arming and"
echo "        disarming under live captures neither hangs the scrub nor trips a"
echo "        lock check, with a disarm landing inside every capture."
