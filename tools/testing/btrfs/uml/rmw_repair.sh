#!/bin/bash
# Does the next write into a damaged stripe repair it -- and refuse when it
# cannot?
#
#   rmw_repair.sh <kernel> [ndev] [fail-device]
#
# A write a device did not take leaves a data column stale, its acknowledged
# value held only by the parity, and the write-intent log naming the column.
# A later read-modify-write of the same full stripe reads the column, sees the
# record, and rebuilds it from the parity to compute the new parity.  It used
# to stop there: the rebuilt sector was dropped, the column stayed stale on
# disk, and the stripe stayed without redundancy until a scrub.  Now the write
# puts it back (rmw_prepare_repair()), under the stripe lock every other write
# to the stripe queues on.
#
# When the record names more of the stripe than the parity can rebuild, no
# parity this write computes can be right: it has to take the stale column as
# it is on disk, which destroys the only copy of what was acknowledged.  The
# write is refused instead.
#
# Boots per arm:
#   prep     a device fails writes under nodatacow overwrites of 'B'; unmount
#   write    rw mount (recovery kept to verifying, so the record reaches the
#            writes), one 4K 'C' written into every one of those full stripes
#   platter  each acknowledged 'B' read straight off its device
#   parity   ro,degraded with the failing device omitted: the 'B' blocks come
#            from the parity alone
#
# Arms:
#   trigger        nothing writes after the faults: the repair queued when the
#                  write failed puts the stale column back once the device is
#                  healthy (trigger-ctl: raid56_no_repair_on_fault=1)
#   repair         0 'B' missing from the platters
#   repair-ctl     raid56_rmw_no_repair=1: the stale 'B's are still missing
#   refuse         every parity recorded unusable (ambiguous): the writes into
#                  those stripes are refused, and the parity still has every 'B'
#   refuse-ctl     raid56_rmw_no_refuse=1: the writes go ahead and the parity
#                  loses the 'B's it was holding
set -u
T=${BTRFS_TEST_DIR:?set BTRFS_TEST_DIR to a scratch directory}
KERNEL=${1:?usage: rmw_repair.sh <kernel> [ndev] [fail-device]}
NDEV=${2:-4}
FAIL=${3:-1}
PROFILE=raid5:raid1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $T/umltest
# A private copy: other scripts overwrite init-final3.sh in place while their
# guests may be reading it.
cp $HERE/init-final3.sh $T/umltest/init-rmw.sh.$$ && mv -f $T/umltest/init-rmw.sh.$$ $T/umltest/init-rmw.sh	# atomic: a guest may be reading it
cp $HERE/nocow_platter.py $T/umltest/nocow_platter.py
ulimit -c 0

MNTPROBE=/dev/ubda; [ "$FAIL" = "0" ] && MNTPROBE=/dev/ubdb

arm() {	# name fakebadpar write-knob prep-extra arm-env [nowrite]
	local tag=rmw-repair-$1 fakebadpar=$2 knob="${3:-}" prepx="${4:-}" env="${5:-}" nowrite="${6:-}" d
	local D=$T/umltest/$tag
	rm -rf $D; mkdir -p $D
	rm -f $T/umltest/results.$tag $T/umltest/nocow.bad.*.$tag $T/umltest/rmw.acked.$tag
	for i in $(seq 0 $((NDEV-1))); do truncate -s 1G $D/disk$i.img; done

	boot() {	# mode omit mntdev [extra]
		local mode=$1 omit=$2 mntdev=$3 extra="${4:-}" ubds=""
		for d in $(seq 0 $((NDEV-1))); do
			case " $omit " in *" $d "*) continue;; esac
			ubds="$ubds ubd$d=$D/disk$d.img"
		done
		timeout 1500 $KERNEL mem=1G rootfstype=hostfs rootflags=/ rw \
			init=$T/umltest/init-rmw.sh $ubds quiet con=null con0=fd:0,fd:1 \
			BTRFS_TEST_DIR=$T MODE=$mode OPTS=rw PROFILE=$PROFILE TAG=$tag \
			MNTDEV=$mntdev NDEV=$NDEV FAIL=$FAIL $env $extra \
			> $D/log.$mode 2>&1
		echo "boot $mode omit=$omit rc=$?" >> $T/umltest/results.$tag
	}

	boot nocow_persist_prep none /dev/mapper/d0 "NOPERSIST=0 FAKEBADPAR=$fakebadpar $prepx"
	[ -n "$nowrite" ] || boot rmw_write none /dev/ubda "btrfs.raid56_recover_legacy=1 $knob"
	boot nocow_platter none /dev/ubda
	boot nocow_probe "$FAIL" $MNTPROBE PROBE=parity
}

get() { cat $T/umltest/$1 2>/dev/null || echo "?"; }
stat_of() {	# tag name
	grep -ho "after writes: .*$2 [0-9]*" $T/umltest/$1/log.rmw_write 2>/dev/null |
		sed "s/.*$2 \([0-9]*\).*/\1/" | tail -1
}
report() {	# name
	local tag=rmw-repair-$1
	ACK=$(get rmw.acked.$tag)
	DISK=$(get nocow.bad.disk.$tag)
	PAR=$(get nocow.bad.parity.$tag)
	REP=$(stat_of $tag rmw_repaired_sectors); REP=${REP:-?}
	REF=$(stat_of $tag rmw_refused); REF=${REF:-?}
	SPLAT=$(grep -l KERNEL_SPLAT $T/umltest/$tag/log.* 2>/dev/null | wc -l)
	echo "  $1: 'C' writes acked=$ACK  repaired sectors=$REP  refused=$REF  'B' missing on platters=$DISK  lost from parity=$PAR  splats=$SPLAT"
}

# The write-path arms keep the triggered repair out of the way, so that what
# they measure is the write's doing and nothing else's.
NOTRIG=btrfs.raid56_no_repair_on_fault=1
# The refuse arms: one 'B' per full stripe (stride = a full stripe of three
# data columns), and the 'C' writes one column either side of it -- the same
# vertical stripe in a neighbouring column, which is the only place a write
# can fold a stale sector into the parity that holds it without overwriting
# it.  Either neighbour may fall in the next full stripe; one of them never
# does.
REFUSE_ENV="NOCOW_STRIDE=48 PREP_SIZE_MB=8 RMW_OFFSETS=16,-16"

arm repair      0 ""                              $NOTRIG
arm repair-ctl  0 btrfs.raid56_rmw_no_repair=1    $NOTRIG
arm refuse      1 ""                              $NOTRIG "$REFUSE_ENV"
arm refuse-ctl  1 btrfs.raid56_rmw_no_refuse=1    $NOTRIG "$REFUSE_ENV"
# The triggered repair: nothing writes after the faults.  The prep heals the
# device and waits; the repair queued on the failed writes must have put the
# stale columns back by itself.
arm trigger     0 "" "SETTLE=20"                  "" nowrite
arm trigger-ctl 0 "" "SETTLE=20 $NOTRIG"          "" nowrite
# A repair that landed must also retire the record, or the log fills with
# stripes that are fine.  The control repairs and keeps it.
arm trigger-keep 0 "" "SETTLE=20 btrfs.raid56_repair_keeps_record=1" "" nowrite

echo "== results =="
report repair;     R_ACK=$ACK R_DISK=$DISK R_PAR=$PAR R_REP=$REP R_SPLAT=$SPLAT
report repair-ctl; RC_DISK=$DISK RC_REP=$REP RC_SPLAT=$SPLAT
report refuse;     F_ACK=$ACK F_PAR=$PAR F_REF=$REF F_SPLAT=$SPLAT
report refuse-ctl; FC_ACK=$ACK FC_PAR=$PAR FC_SPLAT=$SPLAT
report trigger;    T_DISK=$DISK T_PAR=$PAR T_SPLAT=$SPLAT
report trigger-ctl; TC_DISK=$DISK TC_PAR=$PAR TC_SPLAT=$SPLAT
T_OK=$(grep -ho 'after write errors: .*repair_ok [0-9]*' $T/umltest/rmw-repair-trigger/log.nocow_persist_prep 2>/dev/null |
	sed 's/.*repair_ok \([0-9]*\).*/\1/' | tail -1)
sticky_of() {
	grep -ho 'after write errors: .*sticky_blocks [0-9]*' $T/umltest/rmw-repair-$1/log.nocow_persist_prep 2>/dev/null |
		sed 's/.*sticky_blocks \([0-9]*\).*/\1/' | tail -1
}
T_ST=$(sticky_of trigger); K_ST=$(sticky_of trigger-keep)
report trigger-keep; K_DISK=$DISK
echo "  trigger: repairs that succeeded on their own: ${T_OK:-?}; records left: ${T_ST:-?} (kept by the control: ${K_ST:-?})"
echo

case "$R_DISK$R_PAR$RC_DISK$F_PAR$F_ACK$FC_PAR$FC_ACK$T_DISK$T_PAR$TC_DISK" in *'?'*)
	echo "RESULT: INCONCLUSIVE -- a boot did not report"; exit 2;; esac
fails=0
bad() { echo "  FAIL: $1"; fails=$((fails+1)); }
[ $((R_SPLAT + RC_SPLAT + F_SPLAT + FC_SPLAT + T_SPLAT + TC_SPLAT)) -eq 0 ] || bad "kernel splat"
for c in "$RC_DISK:with write-path repair off nothing was stale on the platters" \
	 "$FC_PAR:letting the writes through lost nothing from the parity" \
	 "$TC_DISK:with the triggered repair off nothing was stale on the platters"; do
	if [ "${c%%:*}" -eq 0 ]; then
		echo "RESULT: INCONCLUSIVE -- ${c#*:}, so its arm proves nothing"; exit 2
	fi
done
[ "$R_DISK" -eq 0 ] || bad "the writes left $R_DISK stale block(s) on the platters (control: $RC_DISK)"
[ "$R_PAR" -eq 0 ] || bad "repairing lost $R_PAR block(s) from the parity"
[ "$R_REP" != "?" ] && [ "$R_REP" -gt 0 ] || bad "no sector was counted as repaired"
[ "$R_ACK" -eq 32 ] 2>/dev/null || bad "only $R_ACK of 32 writes were accepted where every stripe was repairable"
[ "$F_PAR" -eq 0 ] || bad "refusing still lost $F_PAR block(s) from the parity (control: $FC_PAR)"
[ "$F_REF" != "?" ] && [ "$F_REF" -gt 0 ] || bad "no write was refused"
[ "$F_ACK" -lt "$FC_ACK" ] 2>/dev/null || bad "the refuse arm accepted $F_ACK writes, as many as the control ($FC_ACK)"
[ "$T_DISK" -eq 0 ] || bad "the triggered repair left $T_DISK stale block(s) (control: $TC_DISK)"
[ "$T_PAR" -eq 0 ] || bad "the triggered repair lost $T_PAR block(s) from the parity"
[ "${T_OK:-0}" -gt 0 ] 2>/dev/null || bad "no triggered repair reported success"
[ "${K_ST:-0}" -gt 0 ] 2>/dev/null || bad "the keep-record control retired its records too, so it proves nothing"
[ "${K_DISK:-1}" -eq 0 ] 2>/dev/null || bad "the keep-record control did not repair, so it is not the control it claims"
[ "${T_ST:-1}" -eq 0 ] 2>/dev/null || bad "${T_ST:-?} record block(s) outlived the repairs that fixed them"
if [ $fails -ne 0 ]; then echo "RESULT: FAIL -- $fails check(s) failed"; exit 1; fi
echo "RESULT: PASS -- the next write repaired the stripe (control left $RC_DISK"
echo "        stale); into an undecidable stripe it was refused and the parity"
echo "        kept everything (control lost $FC_PAR; $FC_ACK accepted against"
echo "        $F_ACK); with nothing writing, the repair queued on the fault put"
echo "        the stripe back by itself (control left $TC_DISK stale) and retired"
echo "        its record (kept: $K_ST)"
